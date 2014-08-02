#include <net/socket.h>
#include <net/epollor.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <cstring>

#include <iostream>

socket::socket():socket_base(), connect_status(PRECONNECT), wrable(false), rdable(false){
	set_noblock();
	rd_worker.task(std::bind(&socket::job, this, std::ref(rd_mutex), 
				std::ref(rd_request)));
	wr_worker.task(std::bind(&socket::job, this, std::ref(wr_mutex),
				std::ref(wr_request)));
}

socket::socket(int fd, std::shared_ptr<struct sockaddr_in> address)
	:socket_base(fd), addr(address), wrable(false), rdable(false), connect_status(CONNECTED){
	set_noblock();
	rd_worker.task(std::bind(&socket::job, this, std::ref(rd_mutex), 
				std::ref(rd_request)));
	wr_worker.task(std::bind(&socket::job, this, std::ref(wr_mutex),
				std::ref(wr_request)));
}

socket::~socket(){}

bool socket::update_addr(const std::string &host, unsigned short port){
	struct hostent *hostent = gethostbyname(host.c_str());
	if(nullptr == hostent){
		return false;
	}
	if(nullptr == addr){
		addr = std::shared_ptr<struct sockaddr_in>(new struct sockaddr_in());
	}
	bzero(addr.get(), sizeof(struct sockaddr_in));

	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	addr->sin_addr = *(reinterpret_cast<struct in_addr*>(hostent->h_addr));

	return true;
}

bool socket::connect(const std::string &ip, unsigned short port){
	if(-1 == sock){
		open();
		register_epoll_req();
	}

	if(!update_addr(ip, port)){
		return false;
	}
	/*if(nullptr == addr){
		addr = std::shared_ptr<struct sockaddr_in>(new struct sockaddr_in);
	}

	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	addr->sin_addr.s_addr = inet_addr(ip.c_str());*/

	std::mutex mutex;
	std::unique_lock<std::mutex> locker(mutex);

	connect_status = PRECONNECT;
	if(-1 == ::connect(sock, 
				reinterpret_cast<struct sockaddr*>(addr.get()), 
				sizeof(struct sockaddr))){
		if(EINPROGRESS == errno){
			connect_status = CONNECTING;
			connect_notify.wait(locker);
			return CONNECTED == connect_status;
		}
		connect_status = CONNECTERR;
		return false;
	}
	connect_status = CONNECTED;
	return true;
}

int socket::sync_write(const void *buff, size_t length){
	int transfered = 0;
	std::mutex sync_mutex;
	std::unique_lock<std::mutex> locker(sync_mutex);
	std::condition_variable cv;
	{
		std::lock_guard<std::mutex> locker(wr_mutex);
		wr_request.push_back(std::bind(&socket::sync_wr_action, this, 
					reinterpret_cast<const char*>(buff), length, std::ref(transfered),
					std::ref(cv)));
		if(wrable)
			wr_worker.active();
	}
	cv.wait(locker);
	
	return transfered;
}

int socket::sync_read(void *buff, size_t length){
	int transfered = 0;
	std::mutex sync_mutex;
	std::unique_lock<std::mutex> locker(sync_mutex);
	std::condition_variable sync_cv;
	{
		std::lock_guard<std::mutex> locker(rd_mutex);
		rd_request.push_back(std::bind(&socket::sync_rd_action, this, 
					reinterpret_cast<char*>(buff), length, std::ref(transfered),
					std::ref(sync_cv)));
		if(rdable)
			rd_worker.active();
	}
	sync_cv.wait(locker);

	return transfered;
}

void socket::async_write(const void *buff, size_t length, ocallback ocb){
	std::lock_guard<std::mutex> locker(rd_mutex);
	wr_request.push_back(
			std::bind(&socket::async_wr_action, this, reinterpret_cast<const char *>(buff), length, ocb));

	if(wrable)
		wr_worker.active();
}

void socket::async_read(void *buff, size_t length, icallback icb){
	std::lock_guard<std::mutex> locker(wr_mutex);
	rd_request.push_back(
			std::bind(&socket::async_rd_action, this, reinterpret_cast<char*>(buff), length, icb));

	if(rdable)
		rd_worker.active();
}

void socket::sync_rd_action(char *buff, size_t length, int &transfered, std::condition_variable &cv){
	transfered = read_some(buff, length);
	cv.notify_all();
}

void socket::sync_wr_action(const char *buff, size_t length, int &transfered, std::condition_variable &cv){
	transfered = write_some(buff, length);
	cv.notify_all();
}

void socket::async_rd_action(char *buff, size_t length, icallback icb){
	int transfered = read_some(buff, length);
	int ec = 0;
	epollor::instance()->get_processor()->arrange(
			std::bind(icb, ec, transfered));
}

void socket::async_wr_action(const char *buff, size_t length, ocallback ocb){
	int transfered = write_some(buff, length);
	int ec = 0;
	epollor::instance()->get_processor()->arrange(
			std::bind(ocb, ec, transfered));
}

inline int socket::read_some(char *buff, size_t length){
	int cnt = 0;
	int nread = 0;

	while(cnt < length && (nread = ::read(sock, buff, length)) > 0){
		cnt += nread;
	}

	if(nread > 0){
		return cnt;
	}
	else if(-1 == nread && EAGAIN == errno){
		rdable = false;
		return cnt;
	}
	else{
		return -1;
	}
}

inline int socket::write_some(const char *buff, size_t length){
	int cnt = 0;
	int nwrite = 0;
	while(cnt < length && (nwrite = ::write(sock, buff, length)) > 0){
		cnt += nwrite;
	}

	if(nwrite > 0){
		return cnt;
	}
	else if(-1 == nwrite && EAGAIN == nwrite){
		wrable = false;
		return cnt;
	}
	else{
		return -1;
	}
}

void socket::job(std::mutex &mutex, std::list<std::function<void()>> &requests){
	switch(connect_status){
		case CONNECTED:
			{
				std::function<void()> callback;
				{
					std::lock_guard<std::mutex> locker(mutex);
					if(requests.empty()){
						int error;
						socklen_t len = sizeof(error);
						if(::getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0){
							std::cerr << "[connected] getsockopt failed!" << std::endl;
							return;
						}
						return;
					}
					callback = requests.front();
					requests.pop_front();
					if(nullptr == callback)
						return;
				}
				callback();
			}
			break;
		case CONNECTING:
			{
				int error;
				socklen_t len = sizeof(error);
				if(::getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0){
					std::cerr << "[preconnect] getsockopt failed!" << std::endl;
					return;
				}
				connect_status = (0 == errno)?CONNECTED:CONNECTERR;
				connect_notify.notify_all();
			}
			break;
	}
}

void socket::ievent(){
	rdable = true;
	rd_worker.active();
}

void socket::oevent(){
	wrable = true;
	wr_worker.active();
}

void socket::rdhupevent(){
	unregister_epoll_req();
	connect_status = PRECONNECT;
	rdable = false;
	wrable = false;
	close();
}

void socket::eevent(){
	if(CONNECTING == connect_status){
		int error;
		socklen_t len = sizeof(error);
		if(::getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0){
			std::cerr << "[preconnect] getsockopt failed!" << std::endl;
			return;
		}
		connect_status = CONNECTERR;
		connect_notify.notify_all();
	}
}
