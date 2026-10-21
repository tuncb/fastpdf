# Refresh provenance on every build, including after a commit without reconfiguration.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${SOURCE_DIR}"
  OUTPUT_VARIABLE COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
configure_file("${SOURCE_DIR}/scripts/release-manifest.json.in"
  "${OUTPUT_DIR}/release-manifest.json" @ONLY)
