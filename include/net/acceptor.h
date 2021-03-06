#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__

#include <net/socket_base.h>
#include <net/socket.h>
#include <string>
#include <functional>
#include <memory>

class acceptor : public socket_base{
	public:
		typedef std::function<void(int, std::shared_ptr<class socket>)> callback;

		acceptor();
		~acceptor();

		bool bind(const std::string &ip, unsigned short port);

		bool listen();

		void accept(callback cb);
	private:
		virtual void ievent();
		virtual void oevent();
		virtual void eevent();

		callback acb;

		bool status;
};

#endif
