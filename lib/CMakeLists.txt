# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_GNSS gnss)
add_subdirectory_ifdef(CONFIG_LORAWAN lorawan_node)
