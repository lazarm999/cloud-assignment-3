#include "CurlEasyPtr.h"
#include <iostream>
#include <sstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include <chrono>
#include <thread>

const unsigned BUF_SIZE = 512;

int ConnectToServer(const char* host, const char* port) {
   // returns socket descriptor connected with process at (host, port) or -1 in case of failure
   int rc;

   addrinfo hints, *listp, *p;  
   std::memset(&hints, 0, sizeof(addrinfo));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if ((rc = getaddrinfo(host, port, &hints, &listp)) != 0) {
      std::cerr << "getaddrinfo error: " << gai_strerror(rc) << std::endl;
      exit(1); 
   }

   int clientsd;

   for (p = listp; p != nullptr; p = p->ai_next) {
      if ((clientsd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) continue;
      if (connect(clientsd, p->ai_addr, p->ai_addrlen) != -1) break; // success
      close(clientsd);
   }

   freeaddrinfo(listp);
   if (!p) return -1; // p is nullptr, all connections failed

   return clientsd;
}

int DoTask(CurlEasyPtr& curl, std::string fileUrl) {
   // Download the file
   curl.setUrl(fileUrl);
   auto csvData = curl.performToStringStream();
   auto result = 0;
   for (std::string row; std::getline(csvData, row, '\n');) {
      auto rowdata = row.data();
      auto limit = rowdata + row.size();
      while ((*rowdata) != '\t' && rowdata < limit) rowdata++;
      rowdata = std::strstr(rowdata, "://");
      if (!rowdata) continue;
      rowdata += 3;
      if (std::strstr(rowdata, "google.ru/") == rowdata) result++;
      // auto rowStream = std::stringstream(std::move(row));
      // // Check the URL in the second column
      // unsigned columnIndex = 0;
      // for (std::string column; std::getline(rowStream, column, '\t'); ++columnIndex) {
      //    // column 0 is id, 1 is URL
      //    if (columnIndex == 1) {
      //       // Check if URL is "google.ru"
      //       auto pos = column.find("://");
      //       if (pos != std::string::npos) {
      //          auto afterProtocol = std::string_view(column).substr(pos + 3);
      //          if (afterProtocol.starts_with("google.ru/"))
      //             ++result;
      //       }
      //       break;
      //    }
      // }
   }
   return result;
}

/// Worker process that receives a list of URLs and reports the result
/// Example:
///    ./worker localhost 4242
/// The worker then contacts the leader process on "localhost" port "4242" for work

int main(int argc, char* argv[]) {
   if (argc != 3) {
      std::cerr << "Usage: " << argv[0] << " <host> <port>" << std::endl;
      return 1;
   }

   // TODO:
   //    1. connect to coordinator specified by host and port
   //       getaddrinfo(), connect(), see: https://beej.us/guide/bgnet/html/#system-calls-or-bust
   //    2. receive work from coordinator
   //       recv(), matching the coordinator's send() work
   //    3. process work
   //       see coordinator.cpp
   //    4. report result
   //       send(), matching the coordinator's recv()
   //    5. repeat

   char buf[BUF_SIZE] = {0};

   sleep(1);
   //std::this_thread::sleep_for(std::chrono::milliseconds(500));

   int clientsd = ConnectToServer(argv[1], argv[2]);
   if (clientsd < 0) {
      exit(1); // unsuccessfull
   }

   auto pid = getpid();
   
   auto curlSetup = CurlGlobalSetup();
   auto curl = CurlEasyPtr::easyInit();

   long msglen;
   while ((msglen = recv(clientsd, buf, sizeof(buf), 0)) > 0) { // while not EOF(0) or error(-1)
      auto fileUrl = std::string(buf);
      auto result = DoTask(curl, fileUrl);
      // Send result
      send(clientsd, &result, sizeof(result), 0);
      //std::cout << fileUrl << std::endl;
      //std::cout << "Process " << pid << std::endl;
   }

   if (msglen < 0) { // error occurred
      std::cerr << pid << " recv() error: " << std::strerror(errno) << std::endl;
      exit(1);
   }
   else if (msglen == 0) {
      //std::cout << "EOF" << std::endl;
   }

   return 0;
}
