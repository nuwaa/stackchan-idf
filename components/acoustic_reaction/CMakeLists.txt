idf_component_register(
    SRCS
        "ggwave_receiver.cpp"
        "reaction_engine.cpp"
        "../../third_party/ggwave/ggwave/ggwave.cpp"
    INCLUDE_DIRS "include" "../../third_party/ggwave"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
