#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <magic_enum.hpp>

#include "LowLatency.hpp"
#include "Bitset.hpp"
#include "LetterBox.hpp"
#include "Protocol.hpp"
#include "SharedMemory.hpp"
#include "String.hpp"
#include "Timestamp.hpp"
#include "Application.hpp"
#include "Tools.hpp"
#include "Json.hpp"

namespace Socket
{
	class SocketChannel
	{
	public:
		static constexpr int32_t AdminChannelLength = Tools::Memory::HugePageLength;
		static constexpr int32_t ExecutionChannelLength = Tools::Memory::HugePageLength;
		static const int32_t Admin = 0;

		// Build the per-direction channel-length array from a CoreGroups bitset (channel index ==
		// CoreGroupId). Size = highest set bit + 1, so unset indices below it become 0-length gaps.
		// Channel 0 = admin (AdminChannelLength); 1..7 = execution (ExecutionChannelLength). Both
		// directions use this same shape. CoreGroupIds must set bit 0 (admin) and stay within 0..7.
		static std::vector<int32_t> BuildChannelLengths(Tools::Bitset64 coreGroupIds)
		{
			coreGroupIds.Set(Admin);
			
			int32_t highest = coreGroupIds.HighestSet();
			if (highest > 7)
				throw std::invalid_argument("SocketChannel::BuildChannelLengths: CoreGroupId > 7 (max 8 channels).");

			std::vector<int32_t> lengths(static_cast<size_t>(highest + 1), 0);
			for (int32_t coreGroupId : coreGroupIds)
				lengths[static_cast<size_t>(coreGroupId)] = (coreGroupId == Admin) ? AdminChannelLength : ExecutionChannelLength;

			return lengths;
		}

		// Per-instrument broadcast ring: one writer (the server), many readers (subscribed clients).
		// Named "{serverName}_{symbol}_data"; Tools::Memory sanitizes the name (spaces/dashes are fine).
		static constexpr int32_t InstrumentDataChannelLength = Tools::Memory::HugePageLength;

		static std::string GetInstrumentDataName(const std::string& serverName, const std::string& symbol)
		{
			return serverName + "_" + symbol + "_data";
		}
	};

	enum class ChannelDirection : uint8_t
	{
		ClientToServer,
		ServerToClient
	};

	class SocketUtils
	{
	public:
		static std::string GetSocketName(const std::string& clientName, const std::string& serverName)
		{
			return "Socket_" + clientName + "_" + serverName;
		}

		static std::string GetChannelName(const std::string& socketName, int32_t channelId, ChannelDirection direction)
		{
			return socketName + "_" + std::string(magic_enum::enum_name(direction)) + "_Channel_" + std::to_string(channelId);
		}
	};

	

#pragma pack(push, 1)
	struct SocketHeader
	{
		Tools::String128 ServerName;
		Tools::String128 ClientName;
		Tools::Timestamp Timestamp;
		int32_t ClientId = -1;
		int32_t ClientProcessId = 0;
		int32_t ClientToServerChannelCount = 0;
		int32_t ServerToClientChannelCount = 0;
		std::array<int32_t, 8> ClientToServerLengths;
		std::array<int32_t, 8> ServerToClientLengths;

		SocketHeader() = default;

		SocketHeader(const std::string& serverName, const std::string& clientName, const std::vector<int32_t>& clientToServerLengths, const std::vector<int32_t>& serverToClientLengths, int32_t clientProcessId)
		{
			ServerName = serverName;
			ClientName = clientName;
			Timestamp = Tools::Timestamp::UtcNow();
			ClientProcessId = clientProcessId;

			ClientToServerChannelCount = std::min(8, static_cast<int32_t>(clientToServerLengths.size()));
			for (int32_t i = 0; i < ClientToServerChannelCount; i++)
			{
				ClientToServerLengths[static_cast<size_t>(i)] = clientToServerLengths[static_cast<size_t>(i)];
			}

			ServerToClientChannelCount = std::min(8, static_cast<int32_t>(serverToClientLengths.size()));
			for (int32_t i = 0; i < ServerToClientChannelCount; i++)
			{
				ServerToClientLengths[static_cast<size_t>(i)] = serverToClientLengths[static_cast<size_t>(i)];
			}
		}

		inline std::string Name() const
		{
			return SocketUtils::GetSocketName(ClientName.ToString(), ServerName.ToString());
		}

		SharedMemory CreateOrOpenSharedMemory() const
		{
			int32_t length = 0;
			for (int32_t i = 0; i < ClientToServerChannelCount; i++)
			{
				length += ClientToServerLengths[static_cast<size_t>(i)];
			}
			for (int32_t i = 0; i < ServerToClientChannelCount; i++)
			{
				length += ServerToClientLengths[static_cast<size_t>(i)];
			}
			return SharedMemory::CreateOrOpen(Name(), length);
		}

        struct glaze
		{
			using T = SocketHeader;
			static constexpr auto value = glz::object(
				"ServerName", &T::ServerName,
				"ClientName", &T::ClientName,
				"Timestamp", &T::Timestamp,
				"ClientId", &T::ClientId,
				"ClientProcessId", &T::ClientProcessId,
				"ClientToServerChannelCount", &T::ClientToServerChannelCount,
				"ServerToClientChannelCount", &T::ServerToClientChannelCount,
				"ClientToServerLengths", &T::ClientToServerLengths,
				"ServerToClientLengths", &T::ServerToClientLengths
			);
		};

		std::string ToString() const
		{
			return Tools::Json::Serialize(this);
		}
	};
#pragma pack(pop)

	enum class ClientStatus : uint8_t
	{
		Disposed = 0,
		Detached = 1,
		Open = 2,
		Closed = 3
	};

	// Forward declaration of Socket to satisfy C++ scope for static constants
	class Socket;

	class ReadOnlySocket
	{
	public:
		static const int32_t BufferSize = 64 * 1024;
		const std::string Name;

	private:
		SharedMemoryView _view;
		std::optional<SharedMemory> _ownedMemory;
		uint8_t* _startPtr = nullptr;
		uint8_t* _readPtr = nullptr;
		uint8_t* _endPtr = nullptr;
		uint64_t _readSeq = 0;
		std::vector<uint8_t> _buffer;
		bool _isClosed = false;
		bool _isDisposed = false;

	public:
		ReadOnlySocket(std::string name, SharedMemoryView view) : Name(std::move(name)), _view(std::move(view))
		{
			_startPtr = _view.Ptr();
			_readPtr = _startPtr;
			_endPtr = _startPtr + _view.Length();
			_buffer.resize(BufferSize);
		}

		// Takes ownership of `memory`: maps the whole region read-only and disposes the SharedMemory
		// when this socket is disposed. Use this when the socket owns its backing region (e.g. a
		// per-instrument broadcast ring); use the view ctor when the region is owned elsewhere.
		ReadOnlySocket(std::string name, SharedMemory memory)
			: ReadOnlySocket(std::move(name), memory.GetView(0, memory.Length(), Tools::Access::Read))
		{
			_ownedMemory.emplace(std::move(memory));
		}

		inline bool IsClosed() const
		{
			return _isClosed;
		}

		inline bool IsDisposed() const
		{
			return _isDisposed;
		}

		inline int32_t Length() const
		{
			return static_cast<int32_t>(_endPtr - _startPtr);
		}

		// Attaches to a ring already in use: park at the head rather than replay the backlog.
		inline void Recover()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");

			Protocol::SkipRing(_readPtr, _startPtr, _endPtr, _readSeq);
			_isClosed = false;   // the previous session's close message is not ours
		}

		inline void Reset()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			
			_readPtr = _startPtr;
			_readSeq = 0;
			_isClosed = false;
		}

		inline void Close()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			
			_isClosed = true;
		}

		inline ReadStatus TryRead(std::span<const uint8_t>& rdst);

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryRead(T& value);

		inline ReadStatus GetReadStatus()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			if (_isClosed)
				return ReadStatus::Closed;
			
			return Protocol::GetReadStatusFromRing(reinterpret_cast<Protocol::Header64*>(_readPtr), _startPtr, _endPtr, _readSeq);
		}

		inline void Dispose()
		{
			if (_isDisposed)
				return;
			
			_isDisposed = true;
			_view.Dispose();
			if (_ownedMemory)
				_ownedMemory->Dispose();
			_startPtr = nullptr;
			_readPtr = nullptr;
			_endPtr = nullptr;
			_buffer.clear();
		}
	};

	class WriteOnlySocket
	{
	public:
		const std::string Name;

	private:
		SharedMemoryView _view;
		std::optional<SharedMemory> _ownedMemory;
		uint8_t* _startPtr = nullptr;
		uint8_t* _endPtr = nullptr;
		uint8_t* _writePtr = nullptr;
		uint64_t _writeSeq = 0;
		bool _isClosed = false;
		bool _isDisposed = false;

	public:
		WriteOnlySocket(std::string name, SharedMemoryView view) : Name(std::move(name)), _view(std::move(view))
		{
			_startPtr = _view.Ptr();
			_writePtr = _startPtr;
			_endPtr = _startPtr + _view.Length();
		}

		// Takes ownership of `memory`: maps the whole region writable and disposes the SharedMemory
		// when this socket is disposed. Use this when the socket owns its backing region (e.g. a
		// per-instrument broadcast ring); use the view ctor when the region is owned elsewhere.
		WriteOnlySocket(std::string name, SharedMemory memory)
			: WriteOnlySocket(std::move(name), memory.GetView(0, memory.Length(), Tools::Access::Write))
		{
			_ownedMemory.emplace(std::move(memory));
		}

		inline bool IsClosed() const
		{
			return _isClosed;
		}

		inline bool IsDisposed() const
		{
			return _isDisposed;
		}

		inline int32_t Length() const
		{
			return static_cast<int32_t>(_endPtr - _startPtr);
		}

		// Attaches to a ring already in use: resume the existing sequence space. Restarting at 0
		inline void Recover()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");

			Protocol::SkipRing(_writePtr, _startPtr, _endPtr, _writeSeq);
			_isClosed = false;   // else Write() emits close messages instead of payloads
		}

		inline void Reset()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			
			_writePtr = _startPtr;
			_writeSeq = 0;
			_isClosed = false;
		}

		inline void Close();

		inline void Write(std::span<const uint8_t> src);

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(const T& value);

		inline void Dispose()
		{
			if (_isDisposed)
				return;
			
			_isDisposed = true;
			_view.Dispose();
			if (_ownedMemory)
				_ownedMemory->Dispose();
			_startPtr = nullptr;
			_writePtr = nullptr;
			_endPtr = nullptr;
		}
	};

	class Socket
	{
	public:
		static const uint8_t CloseMessageByte = 0;
		static inline const std::vector<uint8_t> CloseMessage = { CloseMessageByte };

		static inline bool IsCloseMessage(std::span<const uint8_t> bytes)
		{
			return bytes.size() == 1 && bytes[0] == CloseMessageByte;
		}

		const std::string Name;
		const int32_t ReadChannelCount;
		const int32_t WriteChannelCount;

	private:
		SharedMemory _sharedMemory;
		std::vector<std::unique_ptr<ReadOnlySocket>> _readOnlySockets;
		std::vector<std::unique_ptr<WriteOnlySocket>> _writeOnlySockets;
		bool _isDisposed = false;
		bool _isClosed = false;

	public:
		Socket(std::string name, SharedMemory sharedMemory, const std::vector<SharedMemoryView>& writeViews, const std::vector<SharedMemoryView>& readViews) : Name(std::move(name)), ReadChannelCount(static_cast<int32_t>(readViews.size())), WriteChannelCount(static_cast<int32_t>(writeViews.size())), _sharedMemory(std::move(sharedMemory))
		{
			_writeOnlySockets.resize(writeViews.size());
			_readOnlySockets.resize(readViews.size());

			for (size_t i = 0; i < writeViews.size(); i++)
			{
				if (!writeViews[i].IsDisposed())
				{
					_writeOnlySockets[i] = std::make_unique<WriteOnlySocket>(SocketUtils::GetChannelName(Name, static_cast<int32_t>(i), ChannelDirection::ServerToClient), writeViews[i]);
					_writeOnlySockets[i]->Recover();
				}
			}

			for (size_t i = 0; i < readViews.size(); i++)
			{
				if (!readViews[i].IsDisposed())
				{
					_readOnlySockets[i] = std::make_unique<ReadOnlySocket>(SocketUtils::GetChannelName(Name, static_cast<int32_t>(i), ChannelDirection::ClientToServer), readViews[i]);
					_readOnlySockets[i]->Recover();
				}
			}
		}

		inline bool IsDisposed() const
		{
			return _isDisposed;
		}

		inline bool IsClosed() const
		{
			return _isClosed;
		}

		inline int32_t GetReadChannelLength(int32_t channelId) const
		{
			if (HasReader(channelId))
				return _readOnlySockets[static_cast<size_t>(channelId)]->Length();
			return 0;
		}

		inline int32_t GetWriteChannelLength(int32_t channelId) const
		{
			if (HasWriter(channelId))
				return _writeOnlySockets[static_cast<size_t>(channelId)]->Length();
			return 0;
		}

		inline void Reset()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			
			_isClosed = false;

			for (auto& writer : _writeOnlySockets)
			{
				if (writer)
					writer->Reset();
			}
			for (auto& reader : _readOnlySockets)
			{
				if (reader)
					reader->Reset();
			}
			_sharedMemory.Clear();
		}

		inline void Close()
		{
			if (_isDisposed)
				throw std::runtime_error("ObjectDisposedException");
			
			_isClosed = true;

			for (auto& writer : _writeOnlySockets)
			{
				if (writer)
					writer->Close();
			}
			for (auto& reader : _readOnlySockets)
			{
				if (reader)
					reader->Close();
			}
		}

		inline void Dispose()
		{
			if (_isDisposed)
				return;
			
			_isDisposed = true;

			for (auto& reader : _readOnlySockets)
			{
				if (reader)
					reader->Dispose();
			}
			for (auto& writer : _writeOnlySockets)
			{
				if (writer)
					writer->Dispose();
			}
			_sharedMemory.Dispose();
		}

		inline bool HasReader(int32_t channelId) const
		{
			return channelId >= 0 && channelId < ReadChannelCount && _readOnlySockets[static_cast<size_t>(channelId)] != nullptr;
		}

		inline bool HasWriter(int32_t channelId) const
		{
			return channelId >= 0 && channelId < WriteChannelCount && _writeOnlySockets[static_cast<size_t>(channelId)] != nullptr;
		}

		inline void Write(int32_t channelId, std::span<const uint8_t> bytes)
		{
			if (HasWriter(channelId))
				_writeOnlySockets[static_cast<size_t>(channelId)]->Write(bytes);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(int32_t channelId, const T& value)
		{
			if (HasWriter(channelId))
				_writeOnlySockets[static_cast<size_t>(channelId)]->Write(value);
		}

		inline ReadStatus TryRead(int32_t channelId, std::span<const uint8_t>& bytes)
		{
			if (!HasReader(channelId))
			{
				bytes = {};
				return ReadStatus::Empty;
			}
			return _readOnlySockets[static_cast<size_t>(channelId)]->TryRead(bytes);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryRead(int32_t channelId, T& value)
		{
			if (!HasReader(channelId))
				return ReadStatus::Empty;
			return _readOnlySockets[static_cast<size_t>(channelId)]->TryRead(value);
		}

		inline ReadStatus GetReadStatus(int32_t channelId)
		{
			if (!HasReader(channelId))
				return ReadStatus::Empty;
			return _readOnlySockets[static_cast<size_t>(channelId)]->GetReadStatus();
		}
	};

	// Out-of-line implementations for delayed Socket references
	inline ReadStatus ReadOnlySocket::TryRead(std::span<const uint8_t>& rdst)
	{
		if (_isDisposed)
			throw std::runtime_error("ObjectDisposedException");
		if (_isClosed)
		{
			rdst = {};
			return ReadStatus::Closed;
		}

		std::span<uint8_t> dst(_buffer);
		ReadStatus status = Protocol::TryReadFromRing(_readPtr, _startPtr, _endPtr, dst, rdst, _readSeq);

		if (Socket::IsCloseMessage(rdst))
		{
			_isClosed = true;
			return ReadStatus::Closed;
		}

		return status;
	}

	template <typename T> requires Tools::PlainOldData<T>
	inline ReadStatus ReadOnlySocket::TryRead(T& value)
	{
		if (_isDisposed)
			throw std::runtime_error("ObjectDisposedException");
		if (_isClosed)
		{
			value = {};
			return ReadStatus::Closed;
		}

		std::span<uint8_t> dst(reinterpret_cast<uint8_t*>(&value), sizeof(T));
		std::span<const uint8_t> rdst;
		ReadStatus status = Protocol::TryReadFromRing(_readPtr, _startPtr, _endPtr, dst, rdst, _readSeq);

		if (Socket::IsCloseMessage(rdst))
		{
			_isClosed = true;
			value = T();
			return ReadStatus::Closed;
		}

		if (status != ReadStatus::New)
			return status;

		if (rdst.size() != sizeof(T))
			throw std::runtime_error("Payload length mismatch.");

		return ReadStatus::New;
	}

	inline void WriteOnlySocket::Close()
	{
		if (_isDisposed)
			throw std::runtime_error("ObjectDisposedException");
		if (_isClosed)
			return;
		
		_isClosed = true;
		Protocol::WriteToRing(std::span<const uint8_t>(Socket::CloseMessage.data(), Socket::CloseMessage.size()), _writePtr, _startPtr, _endPtr, _writeSeq);
	}

	inline void WriteOnlySocket::Write(std::span<const uint8_t> src)
	{
		if (_isDisposed)
			throw std::runtime_error("ObjectDisposedException");
		if (_isClosed)
		{
			Protocol::WriteToRing(std::span<const uint8_t>(Socket::CloseMessage.data(), Socket::CloseMessage.size()), _writePtr, _startPtr, _endPtr, _writeSeq);
			return;
		}
		
		Protocol::WriteToRing(src, _writePtr, _startPtr, _endPtr, _writeSeq);
	}

	template <typename T> requires Tools::PlainOldData<T>
	inline void WriteOnlySocket::Write(const T& value)
	{
		if (_isDisposed)
			throw std::runtime_error("ObjectDisposedException");
		if (_isClosed)
			return;
		
		Protocol::WriteToRing(value, _writePtr, _startPtr, _endPtr, _writeSeq);
	}

	class SocketListener
	{
	public:
		const SocketHeader Header;
		const std::string ClientName;
		const std::string ServerName;
		const std::string Name;
		const int32_t ClientToServerChannelCount;
		const int32_t ServerToClientChannelCount;

	private:
		SharedMemory _sharedMemory;
		std::vector<std::unique_ptr<ReadOnlySocket>> _clientToServer;
		std::vector<std::unique_ptr<ReadOnlySocket>> _serverToClient;
		bool _isDisposed = false;

	public:
		SocketListener(const SocketHeader& header) : Header(header), ClientName(header.ClientName.ToString()), ServerName(header.ServerName.ToString()), Name(header.Name()), ClientToServerChannelCount(header.ClientToServerChannelCount), ServerToClientChannelCount(header.ServerToClientChannelCount)
		{
			_sharedMemory = Header.CreateOrOpenSharedMemory();
			_clientToServer.resize(static_cast<size_t>(ClientToServerChannelCount));
			_serverToClient.resize(static_cast<size_t>(ServerToClientChannelCount));

			int32_t offset = 0;
			for (int32_t i = 0; i < ClientToServerChannelCount; i++)
			{
				int32_t len = Header.ClientToServerLengths[static_cast<size_t>(i)];
				if (len > 0)
				{
					_clientToServer[static_cast<size_t>(i)] = std::make_unique<ReadOnlySocket>(SocketUtils::GetChannelName(Name, i, ChannelDirection::ClientToServer), _sharedMemory.GetView(offset, len, Tools::Access::Read));
					offset += len;
				}
			}

			for (int32_t i = 0; i < ServerToClientChannelCount; i++)
			{
				int32_t len = Header.ServerToClientLengths[static_cast<size_t>(i)];
				if (len > 0)
				{
					_serverToClient[static_cast<size_t>(i)] = std::make_unique<ReadOnlySocket>(SocketUtils::GetChannelName(Name, i, ChannelDirection::ServerToClient), _sharedMemory.GetView(offset, len, Tools::Access::Read));
					offset += len;
				}
			}
		}

		inline bool IsDisposed() const
		{
			return _isDisposed;
		}

		inline void Dispose()
		{
			if (_isDisposed)
				return;
			
			_isDisposed = true;

			for (int32_t i = 0; i < ClientToServerChannelCount; i++)
			{
				if (_clientToServer[static_cast<size_t>(i)])
					_clientToServer[static_cast<size_t>(i)]->Dispose();
			}
			for (int32_t i = 0; i < ServerToClientChannelCount; i++)
			{
				if (_serverToClient[static_cast<size_t>(i)])
					_serverToClient[static_cast<size_t>(i)]->Dispose();
			}

			_sharedMemory.Dispose();
		}

		inline bool HasClientToServerChannel(int32_t channelId) const
		{
			return channelId >= 0 && channelId < ClientToServerChannelCount && _clientToServer[static_cast<size_t>(channelId)] != nullptr;
		}

		inline bool HasServerToClientChannel(int32_t channelId) const
		{
			return channelId >= 0 && channelId < ServerToClientChannelCount && _serverToClient[static_cast<size_t>(channelId)] != nullptr;
		}

		inline ReadStatus GetServerToClientReadStatus(int32_t channelId) const
		{
			return HasServerToClientChannel(channelId) ? _serverToClient[static_cast<size_t>(channelId)]->GetReadStatus() : ReadStatus::Empty;
		}

		inline ReadStatus GetClientToServerReadStatus(int32_t channelId) const
		{
			return HasClientToServerChannel(channelId) ? _clientToServer[static_cast<size_t>(channelId)]->GetReadStatus() : ReadStatus::Empty;
		}

		inline ReadStatus TryReadServerToClient(int32_t channelId, std::span<const uint8_t>& bytes)
		{
			if (!HasServerToClientChannel(channelId))
			{
				bytes = {};
				return ReadStatus::Empty;
			}
			return _serverToClient[static_cast<size_t>(channelId)]->TryRead(bytes);
		}

		inline ReadStatus TryReadClientToServer(int32_t channelId, std::span<const uint8_t>& bytes)
		{
			if (!HasClientToServerChannel(channelId))
			{
				bytes = {};
				return ReadStatus::Empty;
			}
			return _clientToServer[static_cast<size_t>(channelId)]->TryRead(bytes);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryReadServerToClient(int32_t channelId, T& value)
		{
			if (!HasServerToClientChannel(channelId))
				return ReadStatus::Empty;
			return _serverToClient[static_cast<size_t>(channelId)]->TryRead(value);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryReadClientToServer(int32_t channelId, T& value)
		{
			if (!HasClientToServerChannel(channelId))
				return ReadStatus::Empty;
			return _clientToServer[static_cast<size_t>(channelId)]->TryRead(value);
		}
	};

	class ClientSocket
	{
	public:
		const std::string ClientName;
		const std::string ServerName;
		const std::string Name;
		const int32_t ClientToServerChannelCount;
		const int32_t ServerToClientChannelCount;

	private:
		SocketHeader _socketHeader;
		std::unique_ptr<Socket> _socket;

	public:
		ClientSocket(const std::string& clientName, const std::string& serverName, const std::vector<int32_t>& clientToServerLengths, const std::vector<int32_t>& serverToClientLengths) : ClientName(clientName), ServerName(serverName), Name(SocketUtils::GetSocketName(clientName, serverName)), ClientToServerChannelCount(static_cast<int32_t>(clientToServerLengths.size())), ServerToClientChannelCount(static_cast<int32_t>(serverToClientLengths.size()))
		{
			for (size_t i = 0; i < clientToServerLengths.size(); i++)
			{
				if (clientToServerLengths[i] > 0 && (clientToServerLengths[i] & 63) != 0)
					throw std::invalid_argument("ClientSocket ClientToServer length must be multiple of 64");
			}

			for (size_t i = 0; i < serverToClientLengths.size(); i++)
			{
				if (serverToClientLengths[i] > 0 && (serverToClientLengths[i] & 63) != 0)
					throw std::invalid_argument("ClientSocket ServerToClient length must be multiple of 64");
			}

			_socketHeader = SocketHeader(serverName, clientName, clientToServerLengths, serverToClientLengths, getpid());

			if (ClientToServerChannelCount > 8 || ServerToClientChannelCount > 8)
				throw std::invalid_argument("Max 8 channels");

			SharedMemory sharedMemory = _socketHeader.CreateOrOpenSharedMemory();

			std::vector<SharedMemoryView> clientToServer(static_cast<size_t>(ClientToServerChannelCount));
			std::vector<SharedMemoryView> serverToClient(static_cast<size_t>(ServerToClientChannelCount));

			int32_t offset = 0;
			for (int32_t i = 0; i < ClientToServerChannelCount; i++)
			{
				if (clientToServerLengths[static_cast<size_t>(i)] > 0)
				{
					clientToServer[static_cast<size_t>(i)] = sharedMemory.GetView(offset, clientToServerLengths[static_cast<size_t>(i)], Tools::Access::Write);
					offset += clientToServerLengths[static_cast<size_t>(i)];
				}
			}

			for (int32_t i = 0; i < ServerToClientChannelCount; i++)
			{
				if (serverToClientLengths[static_cast<size_t>(i)] > 0)
				{
					serverToClient[static_cast<size_t>(i)] = sharedMemory.GetView(offset, serverToClientLengths[static_cast<size_t>(i)], Tools::Access::Read);
					offset += serverToClientLengths[static_cast<size_t>(i)];
				}
			}

			_socket = std::make_unique<Socket>(Name, std::move(sharedMemory), std::move(clientToServer), std::move(serverToClient));
			Tools::Application::AddExitAction("Close ClientSocket " + Name, [this]() { Close(); });
		}

		inline bool IsDisposed() const
		{
			return !_socket || _socket->IsDisposed();
		}

		inline bool IsClosed() const
		{
			return !_socket || _socket->IsClosed();
		}

		inline int32_t Connect()
		{
			if (!_socket)
				throw std::runtime_error("No socket");
			if (IsClosed())
				return -1;

			LetterBox<SocketHeader> clientBox(ClientName, Tools::Access::Read);
			LetterBox<SocketHeader> serverBox(ServerName, Tools::Access::Write);

			while (!serverBox.TryStore(_socketHeader))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			Tools::Timestamp waitingSince = Tools::Timestamp::UtcNow();
			SocketHeader reply;

			while (true)
			{
				bool peek = clientBox.TryPeek(reply);

				if (peek && reply.ClientName == _socketHeader.ClientName && reply.Timestamp == _socketHeader.Timestamp)
				{
					clientBox.TryEmpty(reply);
					break;
				}

				Tools::Timestamp now = Tools::Timestamp::UtcNow();
				if ((now - waitingSince).GetTotalSeconds() > 3)
				{
					waitingSince = now;
					std::cout << ClientName << ": Waiting for server " << ServerName << "..." << std::endl;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			std::cout << ClientName << ": Connected." << std::endl;
			return reply.ClientId;
		}

		inline void Close()
		{
			if (_socket)
				_socket->Close();
		}

		inline void Dispose()
		{
			if (_socket)
				_socket->Dispose();
		}

		inline void Write(std::span<const uint8_t> src)
		{
			Write(0, src);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(const T& value)
		{
			Write(0, value);
		}

		inline void Write(int32_t channelId, std::span<const uint8_t> src)
		{
			_socket->Write(channelId, src);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(int32_t channelId, const T& value)
		{
			_socket->Write(channelId, value);
		}

		inline ReadStatus TryRead(std::span<const uint8_t>& rsrc)
		{
			return TryRead(0, rsrc);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryRead(T& value)
		{
			return TryRead(0, value);
		}

		inline ReadStatus TryRead(int32_t channelId, std::span<const uint8_t>& rsrc)
		{
			return _socket->TryRead(channelId, rsrc);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline ReadStatus TryRead(int32_t channelId, T& value)
		{
			return _socket->TryRead(channelId, value);
		}

		inline ReadStatus GetReadStatus(int32_t channelId = 0)
		{
			return _socket->GetReadStatus(channelId);
		}
	};

	class ServerSocket
	{
	private:
		struct alignas(64) ClientHeader
		{
			std::atomic<ClientStatus> Status{ClientStatus::Disposed};
			std::atomic<int64_t> _closedTimestamp{Tools::Timestamp::MaxValue.NanosSinceEpoch};

			inline Tools::Timestamp GetClosedTimestamp() const
			{
				return Tools::Timestamp(_closedTimestamp.load(std::memory_order_acquire));
			}

			inline void SetClosedTimestamp(Tools::Timestamp value)
			{
				_closedTimestamp.store(value.NanosSinceEpoch, std::memory_order_release);
			}

			int32_t ClientProcessId{0};
			std::unique_ptr<Socket> ClientSocket;
		};

	public:
		bool Persistance = false;
		const int32_t Capacity;
		const std::string ServerName;
		std::function<void(const SocketHeader&)> ClientAllocated;
		std::function<void(const SocketHeader&)> ClientDeallocated;

		std::function<void(int32_t)> ClientOpened;
		std::function<void(int32_t)> ClientClosed;

		std::function<void(std::exception&)> Exception;
		std::function<int32_t(const SocketHeader&)> AllocateClientId;
		std::function<SocketHeader(int32_t)> DeallocateClient;

	private:
		LetterBox<SocketHeader> _letterBox;
		std::vector<SocketHeader> _clientSocketHeaders;
		std::unique_ptr<ClientHeader[]> _clientHeaders;
		Tools::Bitset64 _clientIds; // replace with IBitset so it can handle any capacity
		std::thread _listenThread;
		std::atomic<bool> _isRunning;

	public:
		ServerSocket(std::string name, int32_t capacity) : Capacity(capacity), ServerName(std::move(name)), _letterBox(ServerName, Tools::Access::Write), _isRunning(false)
		{
			_clientSocketHeaders.resize(static_cast<size_t>(capacity));
			_clientHeaders = std::make_unique<ClientHeader[]>(static_cast<size_t>(capacity)); // runs field initializers automatically via make_unique
			
			AllocateClientId = [this](const SocketHeader& header) { return DefaultClientIdAllocator(header); };
			DeallocateClient = [this](int32_t id) { return DefaultClientDeallocator(id); };

			Tools::Application::AddExitAction("Close ServerSocket " + ServerName, [this]() { Dispose(); });
		}

		// Only availalbe if the capacity is 64 or less, otherwise returns garbage.
		inline Tools::Bitset64 ClientIds() const
		{
			return Tools::Bitset64(_clientIds.AtomicLoad());
		}

		inline void Listen()
		{
			if (_isRunning)
				return;
			
			_isRunning = true;
			_listenThread = Tools::LowLatency::StartBackgroundThread(ServerName + ".Listen()", [this]()
			{
				while (_isRunning)
				{
					PollPids();
					PollLetterBox();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
		}

		void CreateDetatchedClient(const SocketHeader& socketHeader)
		{
			if (!Persistance)
				throw std::runtime_error("PersistClients is disabled");

			if (socketHeader.ClientId < 0 || socketHeader.ClientId >= Capacity)
				throw std::invalid_argument("Invalid client id");			

			CreateClient(socketHeader);

			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(socketHeader.ClientId)];
			clientHeader.Status.store(ClientStatus::Detached, std::memory_order_release);
		}

	private:
		int32_t DefaultClientIdAllocator(const SocketHeader& socketHeader)
		{
			for (size_t i = 0; i < _clientSocketHeaders.size(); i++)
			{
				if (_clientSocketHeaders[i].ClientName == socketHeader.ClientName)
					return static_cast<int32_t>(i);
			}

			if (_clientIds.IsFull())
				return -1;

			SocketHeader socketHeaderCopy = socketHeader;
			socketHeaderCopy.ClientId = _clientIds.LowestClear();
			_clientSocketHeaders[static_cast<size_t>(socketHeaderCopy.ClientId)] = socketHeaderCopy;

			return socketHeaderCopy.ClientId;
		}

		SocketHeader DefaultClientDeallocator(int32_t clientId)
		{
			SocketHeader socketHeader = _clientSocketHeaders[static_cast<size_t>(clientId)];
			_clientSocketHeaders[static_cast<size_t>(clientId)] = SocketHeader();
			return socketHeader;
		}

		void DisposeClient(int32_t clientId)
		{
			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(clientId)];

			if (clientHeader.Status.load(std::memory_order_acquire) != ClientStatus::Disposed)
			{
				SocketHeader socketHeader = DeallocateClient(clientId);

				if (ClientDeallocated)
					ClientDeallocated(socketHeader);

				std::cout << ServerName << ": " << socketHeader.ClientName.ToString() << " Disconnected id " << clientId << std::endl;

				clientHeader.Status.store(ClientStatus::Disposed, std::memory_order_release);

				if (clientHeader.ClientSocket)
					clientHeader.ClientSocket->Dispose();

				clientHeader.ClientSocket.reset();
			}
		}

		void CreateClient(const SocketHeader& socketHeader)
		{
			std::string socketName = socketHeader.Name();
			SharedMemory sharedMemory = socketHeader.CreateOrOpenSharedMemory();

			std::vector<SharedMemoryView> clientToServer(static_cast<size_t>(socketHeader.ClientToServerChannelCount));
			std::vector<SharedMemoryView> serverToClient(static_cast<size_t>(socketHeader.ServerToClientChannelCount));

			int32_t offset = 0;
			for (int32_t i = 0; i < socketHeader.ClientToServerChannelCount; i++)
			{
				int32_t len = socketHeader.ClientToServerLengths[static_cast<size_t>(i)];
				if (len > 0)
				{
					clientToServer[static_cast<size_t>(i)] = sharedMemory.GetView(offset, len, Tools::Access::Read);
				}
				offset += len;
			}

			for (int32_t i = 0; i < socketHeader.ServerToClientChannelCount; i++)
			{
				int32_t len = socketHeader.ServerToClientLengths[static_cast<size_t>(i)];
				if (len > 0)
				{
					serverToClient[static_cast<size_t>(i)] = sharedMemory.GetView(offset, len, Tools::Access::Write);
				}
				offset += len;
			}

			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(socketHeader.ClientId)];
			clientHeader.ClientSocket = std::make_unique<Socket>(socketName, std::move(sharedMemory), std::move(serverToClient), std::move(clientToServer));
		}


		void OpenClient(const SocketHeader& socketHeader)
		{
			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(socketHeader.ClientId)];

			if (!Persistance)
				clientHeader.ClientSocket->Reset();

			clientHeader.SetClosedTimestamp(Tools::Timestamp::MaxValue);
			clientHeader.ClientProcessId = socketHeader.ClientProcessId;

			LetterBox<SocketHeader> clientBox(socketHeader.ClientName.ToString(), Tools::Access::Write);

			while (!clientBox.TryStore(socketHeader))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			clientHeader.Status.store(ClientStatus::Open, std::memory_order_release);
			_clientIds.AtomicSet(socketHeader.ClientId);

			if (ClientOpened)
				ClientOpened(socketHeader.ClientId);

			std::cout << ServerName << ": " << socketHeader.ClientName.ToString() << " Connected id " << socketHeader.ClientId << std::endl;
		}

		void CloseClient(int32_t clientId)
		{
			_clientIds.AtomicClear(clientId);
			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(clientId)];
			if (Persistance)
			{
				clientHeader.Status.store(ClientStatus::Detached, std::memory_order_release);
			}
			else
			{
				clientHeader.SetClosedTimestamp(Tools::Timestamp::UtcNow());
				clientHeader.Status.store(ClientStatus::Closed, std::memory_order_release);
			}
			if (ClientClosed)
				ClientClosed(clientId);
		}

		void PollLetterBox()
		{
			SocketHeader socketHeader;

			if (_letterBox.TryEmpty(socketHeader))
			{
				std::string clientName = socketHeader.ClientName.ToString();
				std::cout << "ServerSocket::" << ServerName << ": Received connection request from " << clientName << std::endl;

				int32_t clientId = AllocateClientId(socketHeader);
				socketHeader.ClientId = clientId;

				if (clientId < 0)
				{
					std::cout << "ServerSocket::" << ServerName << ": Client " << clientName << " failed to allocate clientId." << std::endl;
					return;
				}
				ClientStatus status = _clientHeaders[static_cast<size_t>(clientId)].Status.load(std::memory_order_acquire);

				if (status == ClientStatus::Open)
				{
					std::cout << "ServerSocket::" << ServerName << ": Client " << clientName << " is already connected." << std::endl;
					return;
				}
				else if (status == ClientStatus::Closed)
				{
					std::cout << "ServerSocket::" << ServerName << ": Client " << clientName << " is in the process of disposing. Try again in a moment." << std::endl;
					return;
				}
				else if (status == ClientStatus::Detached)
				{

				}
				else if (status == ClientStatus::Disposed)
				{
					if (ClientAllocated)
						ClientAllocated(socketHeader);
					CreateClient(socketHeader);
				}

				OpenClient(socketHeader);
			}
		}

		void PollPids()
		{
			Tools::Timestamp now = Tools::Timestamp::UtcNow();

			for (int32_t clientId = 0; clientId < Capacity; ++clientId)
			{
				ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(clientId)];
				ClientStatus status = clientHeader.Status.load(std::memory_order_acquire);

				if (status == ClientStatus::Open)
				{
					if (!Tools::IsProcessAlive(clientHeader.ClientProcessId))
					{
						CloseClient(clientId);
					}
				}
				else if (status == ClientStatus::Closed)
				{
					if ((now - clientHeader.GetClosedTimestamp()).GetTotalSeconds() > 1)
					{
						DisposeClient(clientId);
					}
				}
			}
		}

	public:
		inline void Stop()
		{
			if (_isRunning)
			{
				_isRunning = false;
				Tools::Join(_listenThread, std::chrono::milliseconds(500));
			}
		}

		inline void Dispose()
		{
			Stop();

			for (int32_t clientId = 0; clientId < Capacity; ++clientId)
			{
				DisposeClient(clientId);
			}

			_letterBox.Dispose();
		}

		inline ReadStatus GetReadStatus(int32_t clientId, int32_t channelId)
		{
			if (clientId < 0 || clientId >= Capacity)
				return ReadStatus::Empty;

			ClientHeader& client = _clientHeaders[static_cast<size_t>(clientId)];

			ClientStatus status = client.Status.load(std::memory_order_acquire);

			if (status != ClientStatus::Open)
				return ReadStatus::Closed;

			try
			{
				if (client.ClientSocket && client.ClientSocket->HasReader(channelId))
				{
					ReadStatus result = client.ClientSocket->GetReadStatus(channelId);

					if (result == ReadStatus::Closed)
					{
						CloseClient(clientId);
					}

					return result;
				}
			}
			catch (std::exception& exception)
			{
				if (Exception)
					Exception(exception);
			}

			return ReadStatus::Empty;
		}

		inline ReadStatus TryRead(int32_t clientId, int32_t channelId, std::span<const uint8_t>& rdst)
		{
			rdst = {};

			if (clientId < 0 || clientId >= Capacity)
				return ReadStatus::Empty;

			ClientHeader& clientHeader = _clientHeaders[static_cast<size_t>(clientId)];

			if (clientHeader.Status.load(std::memory_order_acquire) != ClientStatus::Open || !clientHeader.ClientSocket)
				return ReadStatus::Closed;

			try
			{
				if (!clientHeader.ClientSocket->HasReader(channelId))
					return ReadStatus::Empty;

				ReadStatus result = clientHeader.ClientSocket->TryRead(channelId, rdst);

				if (result == ReadStatus::Closed)
				{
					if (clientHeader.Status.load(std::memory_order_acquire) == ClientStatus::Open)
					{
						CloseClient(clientId);
					}
				}

				return result;
			}
			catch (std::exception& ex)
			{
				if (Exception)
					Exception(ex);
			}

			return ReadStatus::Empty;
		}

		inline ReadStatus TryRead(int32_t clientId, std::span<const uint8_t>& rdst)
		{
			return TryRead(clientId, 0, rdst);
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(int32_t clientId, int32_t channelId, const T& value)
		{
			if (clientId < 0 || clientId >= Capacity)
				return;

			ClientHeader& client = _clientHeaders[static_cast<size_t>(clientId)];
			ClientStatus status = client.Status.load(std::memory_order_acquire);
			bool canWrite = status == ClientStatus::Open || status == ClientStatus::Detached;

			if (!canWrite)
				return;

			try
			{
				if (client.ClientSocket && client.ClientSocket->HasWriter(channelId))
				{
					client.ClientSocket->Write(channelId, value);
				}
			}
			catch (std::exception& ex)
			{
				if (Exception)
					Exception(ex);
			}
		}

		inline void Write(int32_t clientId, int32_t channelId, std::span<const uint8_t> src)
		{
			if (clientId < 0 || clientId >= Capacity)
				return;

			ClientHeader& client = _clientHeaders[static_cast<size_t>(clientId)];

			ClientStatus status = client.Status.load(std::memory_order_acquire);
			bool canWrite = status == ClientStatus::Open || status == ClientStatus::Detached;

			if (!canWrite)
				return;

			try
			{
				if (client.ClientSocket && client.ClientSocket->HasWriter(channelId))
				{
					client.ClientSocket->Write(channelId, src);
				}
			}
			catch (std::exception& ex)
			{
				if (Exception)
					Exception(ex);
			}
		}

		template <typename T> requires Tools::PlainOldData<T>
		inline void Write(int32_t clientId, const T& value)
		{
			Write(clientId, 0, value);
		}

		inline void Write(int32_t clientId, std::span<const uint8_t> src)
		{
			Write(clientId, 0, src);
		}
	};
}