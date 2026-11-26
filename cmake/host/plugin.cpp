// A shared library that translates with cluau (cmake/host/CMakeLists.txt).
#include "cluau/cluau.h"

#include <string>

#if defined(_WIN32)
#define CLUAU_HOST_EXPORT __declspec(dllexport)
#else
#define CLUAU_HOST_EXPORT __attribute__((visibility("default")))
#endif

CLUAU_HOST_EXPORT std::string TranslateForHost(const std::string& source)
{
	return cluau::Translate(source).Luau;
}
