# Third-Party Notices

This document provides copyright and license attributions for third-party software bundled with or transitively included through **Mask**.

Mask itself (`src/`, `include/`, `resources/`, `assets/`) is proprietary software (see [LICENSE](LICENSE)). The components listed below remain the property of their respective copyright holders and are governed by their respective licenses.

---

## 1. HIDMaestro (`lib/HIDMaestro.Core.dll`) — MIT License

- **Component:** `HIDMaestro.Core` (`lib/HIDMaestro.Core.dll`)
- **Version:** `2.1.0.0` (`2.1.0+68040e2ab0654091018b19a6e753b4bf435492d1`)
- **Target Framework:** `.NETCoreApp,Version=v10.0` (`Windows10.0.26100.0`)
- **Description:** HIDMaestro UMDF2 virtual HID controller engine: embed in your app to create exact-fidelity virtual game controllers.
- **Author / Upstream:** `hifihedgehog/HIDMaestro` (<https://github.com/hifihedgehog/HIDMaestro>)
- **License:** MIT License

```text
MIT License

Copyright (c) hifihedgehog and HIDMaestro contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. usbip-win2 (`version 0.9.8.1`, transitive via `HIDMaestro.Core.dll`) — BSD-2-Clause License

- **Component:** `usbip-win2` (`usbip2_ude.inf`, `usbip2_ude.sys`, `usbip2_ude.cat`)
- **Version:** `0.9.8.1`
- **Upstream:** <https://github.com/vadimgrn/usbip-win2>
- **Usage Note:** Embedded within `HIDMaestro.Core.dll` as the virtual USB/IP host controller transport for HIDMaestro composite USB device personas.
- **License:** BSD 2-Clause License

```text
BSD 2-Clause License

Copyright (c) 2021-2026, Vadym Hrynchyshyn <vadimgrn@gmail.com>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 3. DsHidMini (transitive via `HIDMaestro.Core.dll`) — BSD-3-Clause License

- **Component:** `DsHidMini` (HID report descriptor and SXS input/feature report architecture lineage)
- **Upstream:** <https://github.com/nefarius/DsHidMini>
- **Usage Note:** Referenced and reproduced in `HIDMaestro.Core.dll`'s DualShock 3 `dualshock-3-full` SXS HID report descriptor and report translation architecture.
- **License:** BSD 3-Clause License

```text
BSD 3-Clause License

Copyright (c) 2020-2025, Benjamin Höglinger-Stelzer
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
