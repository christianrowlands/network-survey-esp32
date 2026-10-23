idf_component_register(
    SRCS "aggregator.c"
    INCLUDE_DIRS "include"
    REQUIRES dot11_parser ns_proto
)
