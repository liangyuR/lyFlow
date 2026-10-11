execute_process(COMMAND git -C "${LYFLOW_SOURCE_ROOT}" rev-parse HEAD
  RESULT_VARIABLE revision_result OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${LYFLOW_SOURCE_ROOT}" status --porcelain --untracked-files=normal
  RESULT_VARIABLE status_result OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(LENGTH "${revision}" revision_length)
if(NOT revision_result EQUAL 0 OR NOT status_result EQUAL 0 OR dirty OR
   NOT revision_length EQUAL 40 OR NOT revision MATCHES "^[0-9a-f]+$")
  set(revision "")
endif()
file(MAKE_DIRECTORY "${LYFLOW_GENERATED_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/source_revision.h.in"
               "${LYFLOW_GENERATED_DIR}/source_revision.h" @ONLY)
