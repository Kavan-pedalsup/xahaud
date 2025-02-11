#[===================================================================[
   dep: MySQL
   MySQL client library integration for rippled
#]===================================================================]

# Create an IMPORTED target for MySQL
add_library(mysql_client UNKNOWN IMPORTED)

# Find MySQL client library and headers
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

# Set properties on the imported target
if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    set_target_properties(mysql_client PROPERTIES
        IMPORTED_LOCATION "${MYSQL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}"
    )
    
    message(STATUS "Found MySQL include dir: ${MYSQL_INCLUDE_DIR}")
    message(STATUS "Found MySQL library: ${MYSQL_LIBRARY}")
else()
    message(FATAL_ERROR "Could not find MySQL development files. Please install libmysqlclient-dev")
endif()

# Add MySQL backend source to rippled sources
list(APPEND rippled_src
    src/ripple/nodestore/backend/MySQLBackend.cpp)

# Link MySQL to rippled
target_link_libraries(ripple_libs 
    INTERFACE 
        mysql_client
)

# Create an alias target for consistency with other deps
add_library(deps::mysql ALIAS mysql_client)
