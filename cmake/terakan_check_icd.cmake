# Invoked with -P after the Meson build step. Meson reports its own failures, but a build that
# succeeds without producing the driver would otherwise go unnoticed until something tried to load
# it, which is exactly the check bin/terakan-build makes.
if(NOT EXISTS "${TERAKAN_ICD}")
  message(FATAL_ERROR "the Meson build reported success but produced no driver at ${TERAKAN_ICD}")
endif()
message(STATUS "Terakan ICD built: ${TERAKAN_ICD}")
