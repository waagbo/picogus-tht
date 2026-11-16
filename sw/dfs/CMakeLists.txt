# PGDFS: DOS file system redirector service for the USB drive.
# Linked into every firmware that carries the USB host stack (USB_MSC).
# PGBOOT (../bootdisk, experimental): the disk image sector service for the
# boot ROM rides on the PGDFS transport and server, so it is part of the
# same library and present wherever PGDFS=1.
add_library(dfs INTERFACE)

target_sources(dfs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/dfs_transport.c
    ${CMAKE_CURRENT_LIST_DIR}/dfs_server.c
    ${CMAKE_CURRENT_LIST_DIR}/dfs_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/../bootdisk/bootdisk.c
    ${CMAKE_CURRENT_LIST_DIR}/../bootdisk/bd_geometry.c
)

target_include_directories(dfs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../bootdisk
)
