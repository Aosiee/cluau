#include "test.h"

int main()
{
	int ran = 0;
	for (const cluau_test::Case& c : cluau_test::Cases())
	{
		const int before = cluau_test::Failures();
		c.Fn();
		ran++;
		if (cluau_test::Failures() != before)
			std::cerr << "  in: " << c.Name << "\n";
	}
	std::cout << "cluau_tests: " << ran << " case(s), " << cluau_test::Failures() << " failure(s)\n";
	return cluau_test::Failures() ? 1 : 0;
}
