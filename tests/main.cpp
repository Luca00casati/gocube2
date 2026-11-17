#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "platform/fine_sleep.h"

int main(int argc, char **argv) {
	const gc::FineSleep fine; // (the tests' frames sleep a millisecond or two)
	doctest::Context context(argc, argv);
	return context.run();
}
