set(whitespaces_ "[\t\n\r ]*")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/BUILD")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/BUILDROOT")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/RPMS")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/SOURCES")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/SPECS")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/SRPMS")

# make sure that we are using the version of cmake and cpack that we are testing
get_filename_component(cpack_path_ "${CMAKE_CPACK_COMMAND}" DIRECTORY)
set(ENV{PATH} "${cpack_path_}:$ENV{PATH}")

execute_process(COMMAND ${RPMBUILD_EXECUTABLE} --define "_topdir ${CMAKE_CURRENT_BINARY_DIR}/test_rpm" --rebuild ${FOUND_FILE_1}
      RESULT_VARIABLE result_
      ERROR_VARIABLE  error_
      OUTPUT_QUIET
  )

set(output_error_message_
    "\n${RPMBUILD_EXECUTABLE} error: '${error_}';\nresult: '${result_}';\n${output_error_message}")

set(EXPECTED_FILE_CONTENT_ "^/foo${whitespaces_}/foo/test_prog$")

file(GLOB_RECURSE FOUND_FILE_ RELATIVE "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/RPMS" "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/RPMS/*.rpm")
list(APPEND foundFiles_ "${FOUND_FILE_}")
list(LENGTH FOUND_FILE_ foundFilesCount_)

if(foundFilesCount_ EQUAL 1)
  unset(PACKAGE_CONTENT)
  getPackageContent("${CMAKE_CURRENT_BINARY_DIR}/test_rpm/RPMS/${FOUND_FILE_}" "PACKAGE_CONTENT")

  string(REGEX MATCH "${EXPECTED_FILE_CONTENT_}"
      expected_content_list "${PACKAGE_CONTENT}")

  if(NOT expected_content_list)
    message(FATAL_ERROR
      "Unexpected file content!\n"
      " Content: '${PACKAGE_CONTENT}'\n\n"
      " Expected: '${EXPECTED_FILE_CONTENT_}'"
      "${output_error_message_}")
  endif()
else()
  message(FATAL_ERROR
    "Found more than one file!"
    " Found files count '${foundFilesCount_}'."
    " Files: '${FOUND_FILE_}'"
    "${output_error_message_}")
endif()

# check that there were no extra files generated
foreach(all_files_glob_ IN LISTS ALL_FILES_GLOB)
  file(GLOB foundAll_ RELATIVE "${CMAKE_CURRENT_BINARY_DIR}/test_rpm/RPMS" "${all_files_glob_}")
  list(APPEND allFoundFiles_ "${foundAll_}")
endforeach()

list(LENGTH foundFiles_ foundFilesCount_)
list(LENGTH allFoundFiles_ allFoundFilesCount_)

if(NOT foundFilesCount_ EQUAL allFoundFilesCount_)
  message(FATAL_ERROR
      "Found more files than expected! Found files: '${allFoundFiles_}'"
      "${output_error_message_}")
endif()
