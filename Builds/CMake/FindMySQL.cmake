# - Find MySQL
find_path(MYSQL_INCLUDE_DIR
    NAMES mysql.h
    PATHS
        /usr/include/mysql
        /usr/local/include/mysql
        /opt/mysql/mysql/include
    DOC "MySQL include directory"
)

find_library(MYSQL_LIBRARY
    NAMES mysqlclient
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/mysql
        /usr/local/lib/mysql
        /opt/mysql/mysql/lib
    DOC "MySQL client library"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQL
    REQUIRED_VARS
        MYSQL_LIBRARY
        MYSQL_INCLUDE_DIR
)

if(MYSQL_FOUND)
    set(MYSQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR})
    set(MYSQL_LIBRARIES ${MYSQL_LIBRARY})
    
    # Create an imported target
    if(NOT TARGET MySQL::MySQL)
        add_library(MySQL::MySQL UNKNOWN IMPORTED)
        set_target_properties(MySQL::MySQL PROPERTIES
            IMPORTED_LOCATION "${MYSQL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}"
        )
    endif()
    
    mark_as_advanced(MYSQL_INCLUDE_DIR MYSQL_LIBRARY)
else()
    message(FATAL_ERROR "Could not find MySQL development files")
endif()

message(STATUS "Using MySQL include dir: ${MYSQL_INCLUDE_DIR}")
message(STATUS "Using MySQL library: ${MYSQL_LIBRARY}")
