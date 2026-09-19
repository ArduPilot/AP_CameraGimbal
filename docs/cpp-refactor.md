# C++ migration and structure

`pr-cpp` starts from `pr-mt11-fixes` (10aeba9). This is the first migration
step, not a replacement of the camera drivers or protocols.

## Compiler and compatibility boundary

Project-owned camera, web, native capture helper and platform utility sources
use `.cpp` and GNU C++17. Firmware disables exceptions and RTTI. Vendor SDK C
sources (including the MT11 scene and sample libraries) still use the C
compiler. The RTSP dependency retains its existing C++11 build. No SDK source
or binary ABI is converted. Dynamic SDK symbols and kernel structures retain
their existing layouts; compile-time ABI assertions remain enabled.

`CXX` selects the cross C++ compiler and `HOST_CXX` the host compiler.
Camera `CFLAGS` remain shared warning/optimization settings, with the C
language standard removed before applying `CXXFLAGS`. ARM release binaries
link the C++ runtime statically so they do not require newer libraries on the
camera. The Windows native builder uses the same C++ sources and still bundles
its Cygwin dependencies.

The compatibility changes include explicit pointer/enum conversions, ordered
initializers, C-linkage declarations at external boundaries, and C++ atomics.
Objects containing atomics are constructed with checked `new (std::nothrow)`
and destroyed with `delete`; they must not be copied or cleared with `memset`.
Existing allocation failure paths and worker shutdown ordering are preserved.

## First structural changes

* `APC_Camera` is an abstract, non-copyable capability interface. Its immutable
  target implementation reads the existing target headers, which remain the
  single source for firmware, XML, the web UI and Python simulator exports.
* `APC_Lens` provides sensor dimensions, lens type, optical/digital zoom
  capabilities and horizontal `get_FOV()`. Zoom is relative to that lens.
  FOV uses the existing focal-length model or measured wide/tele endpoints;
  it is not a linear division of the angle by the zoom factor. Unknown or
  invalid FOV inputs return zero, preserving the metadata convention.
* Lenses and streams remain separate. A stream has a lens mask and resolution
  mask; recording has its own resolution mask. No single stream resolution is
  substituted for sensor or recording resolution.
* `APC_CameraApp` owns backend, media, protocol servers and the manual-control
  lease. Its destructor handles partial startup and orderly shutdown. Command
  line validation takes place before service ownership starts.
* `APC_StringBuffer` owns web output storage, is non-copyable, and reports
  allocation errors without exceptions. Explicit `release()` transfers a
  completed response to the existing HTTP layer. HTML/URL/log escaping stays
  in this class. Translations have their own header, separate from routing.

The existing `ca_*` driver/control interfaces remain during this migration.
It deliberately does not change wire signs, rate calibration, lens selection,
XML parameter types, recording formats, service paths or installed settings.
Capability queries allocate no memory and do no hardware I/O.

## Recommended next steps

1. Introduce `APC_Media` and backend classes behind the current media API.
   Move worker ownership, SDK lifetime and live reconfiguration together.
   Keep gimbal and image-pipeline interfaces separate: cameras use different
   transports for them, and the simulator must implement both independently.
2. Extract shared configuration descriptors and validation from the web and
   camera app. Have web, MAVLink parameters, XML generation and logging use
   those descriptors, while retaining the existing on-disk keys and parameter
   names. This addresses duplicated capability/validation logic directly.
3. Split web request parsing, authentication/session storage, parameter pages,
   media browsing and firmware updates into small classes. Start at the HTTP
   request/response boundary; keep firmware update transactions isolated from
   ordinary page rendering. Preserve the current per-request language choice.
4. Move remaining run-loop services into explicit owners with narrow
   interfaces. Use scoped file descriptors and mutex guards before changing
   threading or scheduling. Keep asynchronous media work off the gimbal loop.
5. Replace runtime target conditionals with capability queries as each module
   is extracted. Keep target defines for SDK includes, board paths, ABI and
   vendor-only operations that really need compile-time selection.

Follow the ArduPilot style of `APC_` classes, private underscored members,
explicit ownership and capability methods. Avoid broad inheritance trees,
exceptions, RTTI, implicit heap allocation in control loops, and a second
hand-maintained database of camera properties.

## Validation

Run `make release` for all hardware packages, `make -C camera_app test
exposure-test zr10-test`, `make -C web test zr10-test network-status-test`,
`python3 tests/test_z1mini.py`, and the existing XML, overlay and simulator
checks. `tests/test_targets.cpp` checks the capability class on all four
targets, including calibrated FOV and unsupported zoom. The web suite includes
buffer ownership, escaping, reuse and allocation-overflow checks.

The source/compiler change still requires device testing before merging,
especially MT11 USB thermal capture and each vendor SDK pipeline. Linux
cross-compilation is not proof of hardware or Windows runtime behaviour.
