
option(WITH_PYTHON_BINDINGS "Support for HTCondor python bindings" OFF)

if ( WITH_PYTHON_BINDINGS )

  set ( CMAKE_LIBRARY_PATH_ORIG ${CMAKE_LIBRARY_PATH} )
  set ( CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} /usr/lib64 )
  find_package(PythonLibs REQUIRED)
  set ( CMAKE_LIBRARY_PATH CMAKE_LIBRARY_PATH_ORIG)

  include_directories(${PYTHON_INCLUDE_DIRS})

  condor_shared_lib( pyclassad classad.cpp classad_wrapper.h exprtree_wrapper.h )
  target_link_libraries( pyclassad classad ${PYTHON_LIBRARIES} -lboost_python )

  condor_shared_lib( classad_module classad_module.cpp )
  target_link_libraries( classad_module pyclassad -lboost_python ${PYTHON_LIBRARIES} )
  set_target_properties(classad_module PROPERTIES PREFIX "" OUTPUT_NAME classad )

  set_source_files_properties(dc_tool.cpp schedd.cpp PROPERTIES COMPILE_FLAGS -Wno-strict-aliasing)
  condor_shared_lib( condor condor.cpp collector.cpp config.cpp daemon_and_ad_types.cpp dc_tool.cpp export_headers.h old_boost.h schedd.cpp secman.cpp )
  target_link_libraries( condor pyclassad condor_utils -lboost_python ${PYTHON_LIBRARIES} )
  set_target_properties( condor PROPERTIES PREFIX "" )

endif ( WITH_PYTHON_BINDINGS )

