#!/usr/bin/env python3
"""Exercise actual input batching bodies with wire packets and overflow boundaries.

No input device, stream, platform backend or application process is started.
The small endian adapter is the boundary; packet layouts and batching decisions
come from the checked-out production source. Negative controls restore each
inverted overflow predicate independently and must fail the same executable.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def block(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "third-party/moonlight-common-c/src/Input.h"
#include "third-party/moonlight-common-c/src/Limelight.h"
static_assert(sizeof(short) == 2);
namespace util::endian {
template<class T> T swap(T value) {
  auto *bytes = reinterpret_cast<unsigned char *>(&value);
  std::reverse(bytes, bytes + sizeof value);
  return value;
}
template<class T> T big(T value) {
  return std::endian::native == std::endian::little ? swap(value) : value;
}
template<class T> T little(T value) {
  return std::endian::native == std::endian::big ? swap(value) : value;
}
}
static int checks = 0;
void require(bool condition, const char *message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
template<class T> bool identical(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}
'''


CHECKS = r'''
void check_mouse_and_scroll() {
  constexpr std::array<short, 9> values = {-32768, -32767, -256, -1, 0, 1, 255, 32766, 32767};
  for (short a : values) for (short b : values) {
    const int sum = static_cast<int>(a) + static_cast<int>(b);
    const bool fits = sum >= -32768 && sum <= 32767;
    const auto expected = fits ? batch_result_e::batched : batch_result_e::terminate_batch;
    // Exercise both axes separately, including overflow of Y after a valid X.
    for (bool y_axis : {false, true}) {
      NV_REL_MOUSE_MOVE_PACKET dest {}, src {};
      dest.deltaX = util::endian::big<short>(y_axis ? 12 : a);
      src.deltaX = util::endian::big<short>(y_axis ? -3 : b);
      dest.deltaY = util::endian::big<short>(y_axis ? a : 12);
      src.deltaY = util::endian::big<short>(y_axis ? b : -3);
      const auto before = dest, source_before = src;
      require(batch(&dest, &src) == expected, "relative mouse: wrong overflow verdict");
      require(identical(src, source_before), "relative mouse mutated the source packet");
      if (fits) {
        require(util::endian::big(dest.deltaX) == (y_axis ? 9 : sum), "relative mouse X sum changed");
        require(util::endian::big(dest.deltaY) == (y_axis ? sum : 9), "relative mouse Y sum changed");
      } else {
        require(identical(dest, before), "relative mouse partially mutated on overflow");
      }
    }
    NV_SCROLL_PACKET vertical {}, vertical_src {};
    vertical.scrollAmt1 = vertical.scrollAmt2 = util::endian::big(a);
    vertical_src.scrollAmt1 = vertical_src.scrollAmt2 = util::endian::big(b);
    const auto vertical_before = vertical, vertical_src_before = vertical_src;
    require(batch(&vertical, &vertical_src) == expected, "vertical scroll: wrong overflow verdict");
    require(identical(vertical_src, vertical_src_before), "vertical scroll mutated the source packet");
    if (fits) {
      require(util::endian::big(vertical.scrollAmt1) == sum && vertical.scrollAmt1 == vertical.scrollAmt2,
              "vertical scroll lost sum or duplicate amount");
    } else require(identical(vertical, vertical_before), "vertical scroll mutated on overflow");

    SS_HSCROLL_PACKET horizontal {}, horizontal_src {};
    horizontal.scrollAmount = util::endian::big(a);
    horizontal_src.scrollAmount = util::endian::big(b);
    const auto horizontal_before = horizontal, horizontal_src_before = horizontal_src;
    require(batch(&horizontal, &horizontal_src) == expected, "horizontal scroll: wrong overflow verdict");
    require(identical(horizontal_src, horizontal_src_before), "horizontal scroll mutated the source packet");
    if (fits) require(util::endian::big(horizontal.scrollAmount) == sum, "horizontal scroll lost sum");
    else require(identical(horizontal, horizontal_before), "horizontal scroll mutated on overflow");
  }
}

void check_discrete_barriers() {
  NV_REL_MOUSE_MOVE_PACKET motion {};
  motion.header.magic = util::endian::little<std::uint32_t>(MOUSE_MOVE_REL_MAGIC_GEN5);
  motion.deltaX = util::endian::big<short>(5);
  NV_INPUT_HEADER button {};
  button.magic = util::endian::little<std::uint32_t>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
  const auto motion_before = motion;
  require(batch(&motion.header, &button) == batch_result_e::terminate_batch,
          "mouse batching crossed a button press");
  require(identical(motion, motion_before), "button barrier mutated motion");
  NV_INPUT_HEADER key {};
  key.magic = util::endian::little<std::uint32_t>(KEY_DOWN_EVENT_MAGIC);
  require(batch(&key, &key) == batch_result_e::terminate_batch, "keyboard events became batchable");

  NV_MULTI_CONTROLLER_PACKET base {};
  base.activeGamepadMask = 1;
  for (int field = 0; field != 7; ++field) {
    auto dest = base, src = base;
    switch (field) {
      case 0: src.buttonFlags = 1; break;
      case 1: src.buttonFlags2 = 1; break;
      case 2: src.leftTrigger = 1; break;
      case 3: src.rightTrigger = 1; break;
      case 4: dest.leftTrigger = 0xEF; src.leftTrigger = 0xF0; break;
      case 5: dest.rightTrigger = 0xEF; src.rightTrigger = 0xF0; break;
      case 6: src.activeGamepadMask = 3; break;
    }
    const auto before = dest, source_before = src;
    require(batch(&dest, &src) == batch_result_e::terminate_batch, "controller press edge was coalesced");
    require(identical(dest, before), "controller barrier mutated destination");
    dest = source_before; src = before;
    require(batch(&dest, &src) == batch_result_e::terminate_batch, "controller release edge was coalesced");
  }
  auto dest = base, src = base;
  src.controllerNumber = 1;
  require(batch(&dest, &src) == batch_result_e::not_batchable, "controller identity was coalesced");
  dest.leftTrigger = 1; src = dest; src.leftTrigger = 0xEF; src.leftStickX = 1234;
  require(batch(&dest, &src) == batch_result_e::batched && identical(dest, src),
          "continuous controller motion no longer keeps the newest state");
}

int main() {
  try {
    check_mouse_and_scroll();
    check_discrete_barriers();
    std::cout << "input batching: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
'''


def fixture(root):
    source = (root / "src/input.cpp").read_text(encoding="utf-8")
    result = block(source, "enum class batch_result_e {") + ";\n"
    for packet in (
        "PNV_REL_MOUSE_MOVE_PACKET", "PNV_ABS_MOUSE_MOVE_PACKET", "PNV_SCROLL_PACKET",
        "PSS_HSCROLL_PACKET", "PNV_MULTI_CONTROLLER_PACKET", "PSS_TOUCH_PACKET", "PSS_PEN_PACKET",
        "PSS_CONTROLLER_TOUCH_PACKET", "PSS_CONTROLLER_MOTION_PACKET", "PNV_INPUT_HEADER",
    ):
        result += block(source, f"batch_result_e batch({packet} dest,") + "\n"
    return PREAMBLE + result + CHECKS


def compile_and_run(source, root, build, compiler, name):
    cpp = build / f"{name}.cpp"
    exe = build / (f"{name}.exe" if os.name == "nt" else name)
    cpp.write_text(source, encoding="utf-8")
    subprocess.run([compiler, "-std=c++20", "-O0", "-Wall", "-Wextra", "-I" + str(root),
                    str(cpp), "-o", str(exe)], check=True, timeout=60)
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--verify-negative-controls", action="store_true")
    args = parser.parse_args()
    root = args.source.resolve()
    program = fixture(root)
    with tempfile.TemporaryDirectory(prefix="vibepollo-input-batch-") as temporary:
        build = Path(temporary)
        result = compile_and_run(program, root, build, args.compiler, "production")
        print(result.stdout, end="")
        if result.returncode:
            raise SystemExit(result.stderr or result.returncode)
        if args.verify_negative_controls:
            needle = "if (__builtin_add_overflow("
            offsets = [i for i in range(len(program)) if program.startswith(needle, i)]
            if len(offsets) != 4:
                raise SystemExit("Expected four independently tested overflow guards")
            for index, offset in enumerate(offsets):
                changed = program[:offset] + program[offset:].replace(needle, "if (!__builtin_add_overflow(", 1)
                result = compile_and_run(changed, root, build, args.compiler, f"inverted_{index}")
                if result.returncode != 1:
                    raise SystemExit(f"Inverted overflow guard {index} was not rejected: {result.stderr}")
                print(f"negative control {index + 1}: rejected ({result.stderr.strip()})")


if __name__ == "__main__":
    main()
