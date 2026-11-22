#include "check.h"

#include "platform/fine_sleep.h"

int main(int argc, char **argv) {
	const gc::FineSleep fine; // (the tests' frames sleep a millisecond or two)
	return check::run(argc, argv);
}
