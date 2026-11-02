# chiaki-ng core

 [![License: AGPL v3 or later](https://img.shields.io/badge/License-AGPL%20v3%2B-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

chiaki-ng's protocol as a standalone library.

Licensed under the GNU Affero General Public License, version 3 or (at your option) any later version, with additional permissions for linking with OpenSSL and for distribution through application stores such as the Apple App Store. See [LICENSE](LICENSE).

## Disclaimer ⚠️

> [!WARNING]  
> **This is an unofficial project that is not endorsed or certified by Sony Interactive Entertainment LLC.**


This repository contains the core protocol to be imported for use by various frontends and is a **free and open sourced** project **for all projects adhering to the terms of the AGPLv3**.

Please do not use this project if you have no intention of adhering to the AGPLv3. This includes [derivative works](https://www.fsf.org/bulletin/2021/fall/the-fundamentals-of-the-agplv3). 

An example of a derivative work would be this:
- You decide to translate this into another language, say Swift.
- You don't link any code here specifically, but it is essentially a full on translation (derivative in nature), done by pointing claude at it.
- You release it and do not open source the code under the same license
- You deny looking at, or using the code claiming that no code is compiled and linked, despite the existence of files indicating that 2) was performed.

The goal of chiaki-ng has been, and will always be, to provide a free and open-source reference standard for the community to enjoy remote play at no benefit to ourselves. We welcome all who are interested in improving and achieving this standard while adhering to AGPLv3 to contribute.


---

### Credits

With thanks to the following chiaki-ng core contributors
- [Street Pea](https://github.com/streetpea)
- [Florian Märkl](https://git.sr.ht/~thestr4ng3r/chiaki) by basis of his work on chiaki
- [Johannes Baiter](https://github.com/jbaiter)
- [Jamie Bartlett](https://github.com/Nikorag)
- [Joni Bimbashi](https://github.com/jonibim)
- [David Rosca](https://github.com/nowrep)
- [Kyogre](https://github.com/kyogre404)


With special thanks to the following contributors who supplied or contributed to research around the following protocols:

| Contributors | Contributions |
|---|---|
| [grill2010](https://github.com/streamingdv) | OG Chiaki-ng contributor of many protocol specs such as PSN Remote Holepunch and Couch Co-op |
| [TechAnto](https://github.com/TechAntohere) | PS5 Researcher, Android TV fork maintainer, and contributor of protocol specs such as newer Takion protocols and cloud co-op |
| [Forward Technologies LLC](https://github.com/ForWard-Technologies-LLC) | For providing the protocols for remote stream and allowing us to study and reimplement the protocol |
| [kyogre404](https://github.com/kyogre404) | NX optimization |


Based on the original Chiaki by [Florian Märkl](https://git.sr.ht/~thestr4ng3r/chiaki)

The following projects greatly assisted with the implementation of this library, particularly Pylux.
- [Pylux](https://github.com/ForWard-Technologies-LLC/Pylux)
- [Cloudpad](https://github.com/Chazq2023/CloudPad-Android)
- [DS5Dongle](https://github.com/awalol/DS5Dongle)
- [DS5Bridge](https://github.com/SundayMoments/DS5_Bridge)


---

### Just a note

This library uses C++23. It is a re-write of chiaki-ng's library with a goal to separate frontend from the business logic and make it easier to support multiple platforms.

This library generally aims to follow [Domain Driven Design](https://en.wikipedia.org/wiki/Domain-driven_design) to model the business domain accordingly and reduce the interface that is required for any frontend to implement a series of steps. You may find more READMEs in each folder explaining that particular folder layout internally if you wish to understand the business logic that is being abstracted away.

We welcome all to contribute. Before your first pull request is merged, CLA Assistant will ask you to sign the [chiaki-ng Contributor License Agreement](https://gist.github.com/streetpea/d97e8adea343ab183cb31ab78139fcd8). It lets the project keep chiaki-ng under the AGPL with the same OpenSSL and application store permissions, and nothing else.

---

### Supported Platforms

Currently, development is primarily taking place on macOS as the original macOS implementation was broken with macOS 27.

We still intend to work on other platforms in the future and welcome contributions.

| Platform  | State  | 
|---|---|
| macOS (26+)  | Well tested, used in macOS frontend  | 
| iOS (26+) | It compiles and has been linked and proven to run  |
|  Linux |  It compiles and has been linked and proven to run | 
|  Atmosphere (HorizonOS) | Well tested, used in Akira | 
|  Windows | Uncompiled, untested. | 
|  Android | Uncompiled, untested. | 

---

### Developers

For developers:

Documentation is avaliable via doxygen, which will be published at a later date.




