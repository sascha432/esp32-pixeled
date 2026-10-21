# The backup file of the settings and the presets (px/config/backup.hpp). Pure C++, built by the
# host tests as well.
idf_component_register(SRCS "src/backup.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES formats memory)
