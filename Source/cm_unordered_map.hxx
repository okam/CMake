/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifndef CM_UNORDERED_MAP_HXX
#define CM_UNORDERED_MAP_HXX

#include <cmConfigure.h>
#include <cstddef>

#if defined(__GLIBCXX__) && (__GLIBCXX__ < 20080306 || __cplusplus < 201103L)
#include <tr1/unordered_map>
#else
#include <unordered_map>
#endif

namespace cm {

#if (defined(_CPPLIB_VER) && _CPPLIB_VER < 520) ||                            \
  (defined(__GLIBCXX__) && (__GLIBCXX__ < 20080306 || __cplusplus < 201103L))

using namespace std::tr1;

#else

using namespace std;

#endif

} // end namespace cm

#define CM_UNORDERED_MAP cm::unordered_map

#endif
