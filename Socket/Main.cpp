
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

#include "Socket.hpp"

using namespace Socket;
using namespace std::chrono_literals;

static const std::string ServerName = "TestServer";
static const int MaxClients = 64;

struct DataPacket
{
	uint64_t Timestamp;
	uint64_t Value;
};

void ClientProcess()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(1, 10000);

	std::string clientName = "Client_" + std::to_string(getpid());
	
	try
	{
		ClientSocket client(clientName, ServerName, {}, {});
		
		// Random start delay
		std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen) % 1000));
		
		client.Connect();

		// Life duration: 5 to 15 seconds
		auto lifeDuration = std::chrono::seconds(5 + (dis(gen) % 11));
		auto startTime = std::chrono::steady_clock::now();

		while (std::chrono::steady_clock::now() - startTime < lifeDuration)
		{
			DataPacket packet;
			ReadStatus status = client.TryRead(packet);
			
			if (status == ReadStatus::New)
			{
				// Echo back
				client.Write(packet);
			}
			else if (status == ReadStatus::Closed)
			{
				std::cout << clientName << ": Server closed connection." << std::endl;
				break;
			}
			else
			{
				// Wait a bit if nothing to read
				std::this_thread::sleep_for(1ms); // Spin/Yield
			}
		}
		
		client.Close();
		std::cout << clientName << ": Disconnecting normally." << std::endl;
	}
	catch (const std::exception& ex)
	{
		std::cerr << clientName << " Exception: " << ex.what() << std::endl;
	}
}

void WriterThread(ServerSocket* server, std::atomic<bool>* running)
{
	std::mt19937 gen(12345);
	std::uniform_int_distribution<uint64_t> dis;

	while (*running)
	{
		// Broadcast every ms
		auto start = std::chrono::steady_clock::now();
		
		for (int32_t i = 0; i < server->Capacity; ++i)
		{
			DataPacket pkt;
			pkt.Timestamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			pkt.Value = dis(gen);
			
			// This is a bit inefficient if we have many empty slots but Capacity is small enough
			server->Write(i, pkt);
		}
		
		std::this_thread::sleep_until(start + 1ms);
	}
}

void ReaderThread(ServerSocket* server, int32_t clientId, std::atomic<bool>* running)
{
	// Dedication read thread for a client
	// We need to know when the client disconnects to stop this thread.
	// The SocketServer returns ReadStatus::Closed, which we can use.
	
	while (*running)
	{		
		std::span<const uint8_t> readSpan;
		ReadStatus status = server->TryRead(clientId, readSpan);
		
		if (status == ReadStatus::Closed)
		{
			// Client disconnected
			return;
		}
		
		if (status == ReadStatus::New)
		{
		   // We got data. In this test we just consume it.
		   // Maybe print stats occasionally?
		   // const DataPacket* p = reinterpret_cast<const DataPacket*>(readSpan.data());
		   // std::cout << "Server read echo from " << clientId << std::endl;
		}
		else
		{
			std::this_thread::sleep_for(1ms);
		}
	}
}

void ServerProcess(const char* exePath)
{
	try
	{
		std::cout << "Starting Server..." << std::endl;
		ServerSocket server(ServerName, MaxClients);
		std::atomic<bool> running{true};
		std::vector<std::thread> readers(MaxClients); 
		// Note: Using a vector of threads for potential readers.
		// But threads need to be joinable or detached. 
		// If we detach, we can't easily join them on exit. 
		// Better: Launch detached threads in Subscribed callback?
		
		server.ClientAllocated = [&](const ::Socket::SocketHeader& socketHeader)
        {
			std::thread([&, socketHeader]()
			{
				ReaderThread(&server, socketHeader.ClientId, &running);
			}).detach();
		};

		server.Listen();
		
		std::thread writer(WriterThread, &server, &running);
		
		// Client Spawner Logic (Orchestrator part of Server process for simplicity)
		std::thread spawner([&]()
		{
			 std::mt19937 gen(std::random_device{}());
			 std::uniform_int_distribution<> dis(500, 2000); // randomize spawn interval
			 
			 while (running)
			 {
				 // Spawn a client
				 pid_t pid = fork();
				 if (pid == 0)
				 {
					 // Child
					 execl(exePath, exePath, "client", nullptr);
					 exit(1); // Should not reach
				 }
				 else if (pid > 0)
				 {
					 // Parent - clean up zombies periodically?
					 // waitpid(-1, nullptr, WNOHANG);
				 }
				 
				 std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));
			 }
		});
		
		// Main Server Loop - Handle cleanup of zombie processes
		while (true)
		{
			 int status;
			 while (waitpid(-1, &status, WNOHANG) > 0)
			 {
				 // Reaped a dead client
			 }
			 std::this_thread::sleep_for(100ms);
		}
		
		running = false;
		writer.join();
		spawner.join();
		server.Stop();
	}
	catch (std::exception& ex)
	{
		std::cerr << "Server Exception: " << ex.what() << std::endl;
	}
}

int main(int argc, char* argv[])
{
	if (argc > 1 && std::string(argv[1]) == "client")
	{
		ClientProcess();
	}
	else
	{
		// Server Mode
		ServerProcess(argv[0]);
	}
	return 0;
}