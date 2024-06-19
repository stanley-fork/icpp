/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#pragma once

#include <string_view>
#include <vector>

namespace icpp {

void exec_main(std::string_view path, const std::vector<std::string> &deps,
               const char *procfg, int iargc, char **iargv);

}
