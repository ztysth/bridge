# Third-party components

bridge uses dynamically linked Qt 6 and OpenSSL. Linux DEB packages depend on
Ubuntu's runtime packages; Windows and macOS archives include the collected
runtime and its dependency notices under `share/bridge/licenses`.

- Qt base/declarative: LGPL-3.0/GPL/commercial licensing, depending on component.
  See [Qt licensing](https://doc.qt.io/qt-6/licensing.html) and the bundled notices.
  Sources and vcpkg build patches are identified by the committed baseline in
  `vcpkg.json`, at [microsoft/vcpkg](https://github.com/microsoft/vcpkg).
- OpenSSL 3: Apache-2.0; see [OpenSSL license](https://github.com/openssl/openssl/blob/master/LICENSE.txt).
- Catch2: BSL-1.0, test-only; it is not a desktop runtime dependency.

The release dependency inventory records exact installed versions. Notices for
transitive runtime dependencies are copied from the SDK's installed share tree.
Qt libraries remain dynamically replaceable; no restriction on debugging a
modified Qt runtime is imposed. System-package notices and corresponding sources
are provided through Ubuntu's package/source repositories.

bridge's source code is licensed under BSD-3-Clause; see [LICENSE](LICENSE).
