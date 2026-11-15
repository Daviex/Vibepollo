#!/usr/bin/env python3
"""Exercise real alloc/reset/deinit bodies on the production executor with inert OS devices."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function, production_prelude


def fixture():
    source = (ROOT / 'src/input.cpp').read_text(encoding='utf-8')
    prelude = production_prelude(source)
    begin = prelude.index('struct worker_t {')
    end = prelude.index('namespace thread_pool_util', begin)
    prelude = prelude[:begin] + '''
using worker_t = input::serial_executor_t;
worker_t input_executor;
''' + prelude[end:]
    prelude = '#include "src/input_executor.h"\n#include <future>\n' + prelude
    prelude = prelude.replace('  using input_t = int;', '''
  struct input_t {
    bool alive = true;
    operator int() const { assert(alive); return 0; }
    void reset() { alive = false; }
  };
  struct deinit_t { virtual ~deinit_t() = default; };
  std::thread::id worker_id;
  int allocations = 0;
''')
    prelude = prelude.replace('~client_input_t() { ++destroyed; }',
                              '~client_input_t() { assert(std::this_thread::get_id() == worker_id); ++destroyed; }')
    prelude = prelude.replace('int platf_input;', 'platf::input_t platf_input;')
    prelude = prelude.replace('auto mouse_controller = &controller;',
                              'auto mouse_controller = std::make_unique<controller_t>();\nauto mouse_backend = std::make_unique<int>(0);')
    prelude = prelude.replace('  const int guide_button_key;', '''
  input_t(int, int, input::guide_button_config_t button): input_t(button) {}
  const int guide_button_key;''')
    boundary = r'''
using input::guide_button_config_t;
namespace input { struct touch_port_t {}; }
namespace platf {
  using gamepad_feedback_msg_t = int;
  constexpr int GAMEPAD_FEEDBACK_QUEUE_CAPACITY = 256;
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &) {
    assert(std::this_thread::get_id() == worker_id);
    ++allocations;
    return std::make_unique<client_input_t>();
  }
}
namespace mail { constexpr int touch_port = 0, gamepad_feedback = 1; }
namespace safe {
  struct mail_raw_t {
    template<class T> int event(int) { return 0; }
    template<class T> int queue(int, int, bool) { return 0; }
  };
  using mail_t = std::shared_ptr<mail_raw_t>;
}
void start_input_geometry_refresh(const std::shared_ptr<input_t> &input) {
  // Geometry mapping itself is covered separately; retain a session-owning
  // future timer to exercise real executor cancellation and capture lifetime.
  input_executor.pushDelayed([input] { assert(false); }, 24h);
}
'''
    handlers = '\n'.join(function(source, signature) for signature in (
        'uint16_t host_keycode(uint16_t key)',
        'void release_key(const held_key_t &key)',
        'void free_gamepad(platf::input_t &platf_input, int id)',
        'void reset_gamepad(gamepad_t &gamepad)',
        'void reset(std::shared_ptr<input_t> &input)',
        'std::shared_ptr<input_t> alloc(safe::mail_t mail,',
    ))
    deinit = function(source, 'class deinit_t: public platf::deinit_t {') + ';\n'
    return prelude + boundary + handlers + deinit + TESTS


TESTS = r'''
void wait(std::future<void> &ready) {
  assert(ready.wait_for(5s) == std::future_status::ready);
  ready.get();
}
void barrier() {
  std::promise<void> ready;
  auto future = ready.get_future();
  assert(input_executor.push([&] { ready.set_value(); }));
  wait(future);
}
void seed(const std::shared_ptr<input_t> &input) {
  assert(input_executor.push([input] {
    input->keys[65] = {65, 0, 0};
    host_keys[65] = {1, 65, 0};
    mouse_press[1] = true;
    mouse_press_owner[1] = input.get();
    gamepadMask[0] = true;
    input->gamepads[0].id = 0;
    input->gamepads[0].synthetic_home_down = true;
    input->gamepads[0].home_release_id = input_executor.pushDelayedRelease([] { assert(false); }, 24h).task_id;
  }));
  barrier();
}
int main(int argc, char **argv) {
  assert(argc == 2);
  input_executor.start([] { platf::worker_id = std::this_thread::get_id(); });
  auto lifetime = std::make_unique<deinit_t>();
  auto mail = std::make_shared<safe::mail_raw_t>();
  std::string scenario = argv[1];
  if (scenario == "reset_retained_context") {
    auto input = alloc(mail, {0, 0}, false);
    seed(input);
    reset(input);
    barrier();
    assert(input->input_closed && !input->client_context);
    assert(input->gamepads[0].id == -1 && !input->gamepads[0].synthetic_home_down);
    assert(active_inputs.empty() && host_keys.empty());
    assert(platf::client_input_t::destroyed == 1);
    lifetime.reset();
  } else if (scenario == "shutdown_retained_and_abandoned_contexts") {
    auto retained = alloc(mail, {0, 0}, false);
    seed(retained);
    auto abandoned = alloc(mail, {0, 0}, false);
    barrier();
    std::weak_ptr<input_t> weak = abandoned;
    abandoned.reset();
    assert(!weak.expired()); // Registry owns cleanup even without transport reset.
    lifetime.reset();
    assert(weak.expired() && retained->input_closed && !retained->client_context);
    assert(platf::client_input_t::destroyed == 2);
    assert(host_keys.empty() && !mouse_press[1] && !gamepadMask[0]);
  } else if (scenario == "shutdown_before_session_initialization") {
    std::promise<void> entered, unblock;
    auto entered_future = entered.get_future(), unblock_future = unblock.get_future();
    assert(input_executor.push([&] { entered.set_value(); unblock_future.wait(); }));
    wait(entered_future);
    auto input = alloc(mail, {0, 0}, false);
    auto stopped = std::async(std::launch::async, [&] { lifetime.reset(); });
    for (;;) {
      std::lock_guard lock(input->input_queue_lock);
      if (input->input_closed) break;
      std::this_thread::yield();
    }
    unblock.set_value();
    assert(stopped.wait_for(5s) == std::future_status::ready);
    stopped.get();
    assert(platf::allocations == 0 && !input->client_context);
  } else { assert(false); }
  assert(!platf_input.alive);
  assert(!input_executor.push([] { assert(false); }));
  auto late = alloc(mail, {0, 0}, false);
  assert(late->input_closed && !late->client_context && active_inputs.empty());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='input-lifecycle-') as temporary:
        cpp, exe = Path(temporary) / 'test.cpp', Path(temporary) / 'test.exe'
        cpp.write_text(fixture(), encoding='utf-8')
        subprocess.run([args.compiler, '-std=c++20', '-pthread', '-Wall', '-Wextra',
                        '-Wno-sign-compare', '-I', str(ROOT), str(cpp), '-o', str(exe)], check=True)
        for scenario in ('reset_retained_context', 'shutdown_retained_and_abandoned_contexts',
                         'shutdown_before_session_initialization'):
            subprocess.run([str(exe), scenario], check=True, timeout=10)
            print('PASS:', scenario)


if __name__ == '__main__':
    main()
