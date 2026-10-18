# ArtifactDeps.cmake — the fetched-library provenance that meta.json records.
#
# A fetched library is recorded as <repo>@<branch|tag> (a branch ref: a rebuild takes its tip), from the
# same cache variables FetchContent_Declare read, so meta.json's `toolchain.<lib>` names what was fetched.
# A local checkout override (FETCHCONTENT_SOURCE_DIR_<NAME>, or an engine-specific <LIB>_SOURCE_DIR) is
# recorded as `local:<path>`, since such an artifact cannot be rebuilt from the descriptor.
#
#   ufront_artifact_dep(<list-var> <label> <fetchcontent-name> <repo> <ref> [<local-source-dir>])
#     appends "--dep-<label>=<repo>@<ref>" (or "--dep-<label>=local:<dir>") to <list-var>, the argument
#     form Unreal/cmake/emit-artifact-meta.mjs takes.
#
# FetchContent re-fetches a branch ref on every configure (FETCHCONTENT_UPDATES_DISCONNECTED is OFF by
# default), never on a plain `cmake --build`.
function(ufront_artifact_dep out label fcname repo ref)
  string(TOUPPER "${fcname}" _u)
  set(_local "${FETCHCONTENT_SOURCE_DIR_${_u}}")
  if(NOT _local AND ARGC GREATER 5)
    set(_local "${ARGV5}")
  endif()
  if(_local)
    set(_spec "local:${_local}")
  else()
    set(_spec "${repo}@${ref}")
  endif()
  list(APPEND ${out} "--dep-${label}=${_spec}")
  set(${out} "${${out}}" PARENT_SCOPE)
endfunction()
