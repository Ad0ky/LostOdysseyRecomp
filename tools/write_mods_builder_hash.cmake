# Hashes the code that rebuilds game files for model and text mods
# (LostOdysseyRecomp/modding/text_overlay.cpp). The rebuilt files in cache/mods
# are keyed on it: a build with the same code keeps the cache, any change to that
# code rebuilds it. SOURCES names a file that lists the sources, one per line.
file(STRINGS "${SOURCES}" sources)
set(combined "")
foreach(source IN LISTS sources)
    file(SHA256 "${source}" digest)
    file(RELATIVE_PATH name "${SOURCE_ROOT}" "${source}")
    string(APPEND combined "${name} ${digest}\n")
endforeach()
string(SHA256 hash "${combined}")
string(SUBSTRING "${hash}" 0 16 hash)
get_filename_component(directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${directory}")
file(WRITE "${OUTPUT}" "// Generated at build time. Do not edit.\n#define LO_MODS_BUILDER_HASH \"${hash}\"\n")
