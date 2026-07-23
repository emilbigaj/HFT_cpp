#pragma once

#include "Socket.hpp"
#include "Tools.hpp"
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <memory>
#include <stdexcept>

namespace Provider
{
	class Loggable
	{
	private:
		static Socket::SocketHeader s_socketHeader;


	public:
		int32_t Length;
		std::filesystem::path FilePath;
		std::unique_ptr<Socket::ClientSocket> ClientSocket;

		Loggable(const std::filesystem::path& filePath, const std::filesystem::path& loggingServerName, int32_t length) : Length(length), FilePath(filePath)
		{
			if (filePath.native().size() > s_socketHeader.ClientName.Capacity)
				throw std::invalid_argument(std::format("Loggable FilePath: {} is too long, must be <= {} chars", filePath.string(), s_socketHeader.ClientName.Capacity));		

			std::vector<int32_t> logClientToServerLengths = { length };
			std::vector<int32_t> logServerToClientLengths = {  };

			ClientSocket = std::make_unique<Socket::ClientSocket>(FilePath, loggingServerName, logClientToServerLengths, logServerToClientLengths);
		}

		virtual ~Loggable() = default;

		void Connect()
		{
			ClientSocket->Connect();
		}
	};

	class LoggableManager
	{
	public:
		std::unordered_map<std::filesystem::path, std::unique_ptr<Loggable>> _loggables;

		void OnLogging(std::unique_ptr<Loggable> logging)
        {
            auto [it, inserted] = _loggables.try_emplace(logging->FilePath, std::move(logging));
            if (!inserted)
            {
                throw std::invalid_argument("Duplicate filePath " + it->first.string());
            }
            it->second->Connect();
        }

	};

	template <typename T> requires Tools::PlainOldData<T>
	class Series : public Loggable
	{
	public:
		T Value;

		Series(const std::filesystem::path& filePath, const std::filesystem::path& loggingServerName) : Loggable(filePath, loggingServerName, Tools::Memory::HugePageLength), Value()
		{
		}

		void Append(const T& value)
		{
			Value = value;
			ClientSocket->Write(value);
		}
	};
    
}
