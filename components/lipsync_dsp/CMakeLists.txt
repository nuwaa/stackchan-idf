idf_component_register(
    SRCS "vowel_detector.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
