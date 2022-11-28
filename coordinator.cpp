#include "CurlEasyPtr.h"
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <mutex>
#include <unordered_map>

using namespace std::literals;

const int LISTENQ = 128;
const int TIMEOUT = 12000; // in ms

struct client_descriptor {
   std::string file;
   std::chrono::steady_clock::time_point last_seen;
   client_descriptor(std::string& fileUrl) {
      file = fileUrl;
      last_seen = std::chrono::steady_clock::now();
   }
   client_descriptor() {
      file = "";
      last_seen = std::chrono::steady_clock::now();
   }
};

int OpenListenSd(const char* port) {
   if (!port) return -1;
   int listensd, optval = 1, rv;
   addrinfo hints, *listp, *p;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE; //   | AI_ADDRCONFIG | AI_NUMERICSERV;
   if ((rv = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
      std::cerr << "getaddrinfo error: " << gai_strerror(rv) << std::endl;
      exit(1);
   }

   for (p = listp; p != nullptr; p = p->ai_next) {
      if ((listensd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) continue;
      /* Eliminates "Address already in use" error from bind */
      setsockopt(listensd, SOL_SOCKET, SO_REUSEADDR, &optval , sizeof(int));
      if (bind(listensd, p->ai_addr, p->ai_addrlen) == 0) break; // success
      close(listensd);
   }
   freeaddrinfo(listp);
   if (!p) {
      return -1;
   }
   if (listen(listensd, LISTENQ) < 0) {
      close(listensd);
      return -1;
   }
   return listensd;
}

long SendTask(int connsd, std::vector<std::string>& unassignedFiles, std::unordered_map<int, client_descriptor>& clients) {
   auto fileUrl = unassignedFiles.back();
   clients[connsd].file = fileUrl;

   ssize_t sentlen;
   auto urllen = fileUrl.size();
   if ((sentlen = send(connsd, fileUrl.data(), urllen, 0)) <= 0) {
      //std::cout << "Here " << sentlen << std::endl;
      clients.erase(connsd);
      close(connsd);
      return -1l;
   }

   unassignedFiles.pop_back();
   clients[connsd].last_seen = std::chrono::steady_clock::now();
   return sentlen;
}

int RecvResult(int connsd, std::vector<std::string>& unassignedFiles, std::unordered_map<int, client_descriptor>& clients) {
   int result;
   if (recv(connsd, &result, sizeof(result), 0) < 0) {
      auto fileUrl = clients[connsd].file;
      clients.erase(connsd);
      unassignedFiles.push_back(fileUrl);
      close(connsd);
      return -1;
   }
   clients[connsd] = {}; // resets the info structure

   return result;
}

int AddToPoll(pollfd* psds, int newsd, int& sd_count, int sd_size)
{
    if (sd_count == sd_size) {
        return 0;
    }

    memset(&psds[sd_count], 0, sizeof(pollfd));
    psds[sd_count].fd = newsd;
    psds[sd_count].events = POLLIN | POLLOUT; // Check ready-to-read and ready-to-write

    sd_count++;
    return 1;
}

// Remove an index from the set
void DelFromPollsds(pollfd* psds, int i, int& sd_count)
{
    // Copy the one from the end over this one
    psds[i] = psds[sd_count-1];
    sd_count--;
}

/// Leader process that coordinates workers. Workers connect on the specified port
/// and the coordinator distributes the work of the CSV file list.
/// Example:
///    ./coordinator http://example.org/filelist.csv 4242

int main(int argc, char* argv[]) {
   if (argc != 3) {
      std::cerr << "Usage: " << argv[0] << " <URL to csv list> <listen port>" << std::endl;
      return 1;
   }

   // TODO:
   //    1. Allow workers to connect
   //       socket(), bind(), listen(), accept(), see: https://beej.us/guide/bgnet/html/#system-calls-or-bust
   //    2. Distribute the following work among workers
   //       send() them some work
   //    3. Collect all results
   //       recv() the results
   // Hint: Think about how you track which worker got what work

   auto curlSetup = CurlGlobalSetup();

   auto listUrl = std::string(argv[1]);

   // Download the file list
   auto curl = CurlEasyPtr::easyInit();
   curl.setUrl(listUrl);
   auto fileList = curl.performToStringStream();

   auto remainingFileNo = 0u;
   std::vector<std::string> unassignedFiles;
   size_t total = 0;
   // Iterate over all files and mark them as unassigned
   for (std::string url; std::getline(fileList, url, '\n');) {
      unassignedFiles.push_back(url);
      remainingFileNo++;
   }

   pollfd* psds = new pollfd[LISTENQ + 1];
   int listensd = OpenListenSd(argv[2]);

   //std::cout << unassignedFiles.front() << "\t" << unassignedFiles.back() << "\t" << remainingFileNo << std::endl;

   psds[0].fd = listensd; psds[0].events = POLLIN;
   auto psdlen = 1;

   std::unordered_map<int, client_descriptor> clients;

   while (remainingFileNo > 0) {
      int poll_count = poll(psds, psdlen, 1000); 
      if (poll_count == -1) {
         perror("poll");
         exit(1);
      }
      for (int i = 0; i < psdlen; i++) { // for each polled socket
         if (psds[i].fd == listensd) {
            if ((psds[i].revents & POLLIN) && psdlen <= LISTENQ+1) {
               sockaddr_storage clientaddr;
               socklen_t clientlen;    
               // accept connection with worker
               int connsd = accept(listensd, (sockaddr*)&clientaddr, &clientlen);
               // add socket descriptor to polling list
               AddToPoll(psds, connsd, psdlen, LISTENQ+1);
               clients[connsd] = {};
            }
        }
        else {
            auto connsd = psds[i].fd;
            auto now = std::chrono::steady_clock::now();
            if (psds[i].revents & POLLIN) {
               auto result = RecvResult(connsd, unassignedFiles, clients);
               if (result != -1) {
                  total += result;
                  remainingFileNo--;
                  //std::cout << "Total: " << total << "\t Remaining: " << remainingFileNo << std::endl;
                  if (!remainingFileNo) break;
               }
            }
            else if ((psds[i].revents & POLLHUP) || std::chrono::duration_cast<std::chrono::milliseconds>(now - clients[connsd].last_seen).count() > TIMEOUT) {
               //std::cout << "Here" << (psds[i].revents) << std::endl;
               auto fileUrl = clients[connsd].file;
               clients.erase(connsd);
               if (!fileUrl.empty()) unassignedFiles.push_back(fileUrl);
               DelFromPollsds(psds, i, psdlen);
               close(connsd);
               i--;
               continue;
            }
            else if ((psds[i].revents & POLLOUT) && clients[connsd].file.empty() && !unassignedFiles.empty()) {
               //std::cout << "File: " << remainingFileNo << ", unassigned len: " << unassignedFiles.size() << std::endl;
               if (SendTask(connsd, unassignedFiles, clients) == -1) {
                  DelFromPollsds(psds, i, psdlen);
               }
            }
        }
      }
   }

   for (int i=0; i<psdlen; i++) close(psds[i].fd);

   std::cout << total << std::endl;
   
   close(listensd);
   delete[] psds;
   return 0;
}
