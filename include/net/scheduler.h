#ifndef __PROCESSOR_H__
#define __PROCESSOR_H__

#include <net/worker.h>
#include <vector>
#include <memory>

class scheduler{
	public:
		scheduler();
		~scheduler();

		bool arrange(std::function<void()> proc);
	private:
		std::vector<std::shared_ptr<worker>> workers;
};

#endif
