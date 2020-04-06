idf_build_get_property(project_dir PROJECT_DIR)
file(GLOB srcs *.c)
set(COMPONENT_REQUIRES homekit)
idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES ${project_dir}/web/wifi.html.gz
                    )