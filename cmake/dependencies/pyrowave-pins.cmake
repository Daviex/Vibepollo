# Update these pins together after checking API/bitstream compatibility. Granite
# is the revision selected by PyroWave's checkout_granite.sh; the other revisions
# are the gitlinks in that Granite commit. Only these two submodules are needed
# for the standalone C API. Shader bytecode is already embedded in PyroWave's
# shaders/slangmosh.hpp at the same source revision; no shader compiler download.
set(SUNSHINE_PYROWAVE_REVISION "d2997ac172bdc00e29c58e3f2938acb7e94580bf")
set(SUNSHINE_PYROWAVE_GRANITE_REVISION "9d44761debb9ac31d8d800cac8b030a7a0390b7e")
set(SUNSHINE_PYROWAVE_VOLK_REVISION "47cddf7ed97b94118a08aacb548a411188e016cc")
set(SUNSHINE_PYROWAVE_VULKAN_HEADERS_REVISION "11d6898377797e07dbd543aaaa367e4465074597")
set(SUNSHINE_PYROWAVE_ARCHIVE_SHA256 "f571c94512225509b3b5c73caa99cb79a1100d19facc390553338e47234163f0")
set(SUNSHINE_PYROWAVE_GRANITE_ARCHIVE_SHA256 "286823300b7ee8b49694361287e3bf37c36c1e4c3663860d9f09655f1d860498")
set(SUNSHINE_PYROWAVE_VOLK_ARCHIVE_SHA256 "ed771f9132ea077af0abc74d29f415a81f181f83c3ac5e0f67b2f2149976fe2d")
set(SUNSHINE_PYROWAVE_VULKAN_HEADERS_ARCHIVE_SHA256 "162e7e95e101dfe3118bef4abd0dd2a38f8308e5c61f60bfcef8c0e8783fc48c")

# Local runtime fixes and an additive bundle-identity export. The upstream API
# signatures and encoded bitstream remain pinned to the revisions above.
set(SUNSHINE_PYROWAVE_PATCH_FILE "${CMAKE_CURRENT_LIST_DIR}/../patches/pyrowave-0.5.0-nt-handle-ownership.patch")
set(SUNSHINE_PYROWAVE_PATCH_SHA256 "ba9f00d2fda290d4fd93d5792b3f08100eb12e9e8fb92add5012a2e76302fc8d")
