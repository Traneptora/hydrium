# hydrium
Hydrium is a fast, ultra-low-memory, streaming JPEG XL encoder written in portable C.

At the moment, it is a work-in-progress and it is still in the early stages of development. The API and CLI are unstable and subject to change without notice.

The design goals of hydrium prioritize streamability and very low memory footprint. By default, libhydrium uses approximately 1.5 megabytes of RAM for images of any size. Tiles can be sent one at a time to the encoder, while will encode them independently.

Hydrium does not use threading or any platform-specific assembly. It is desgined to be as portable as possible so it can be used on low-power embedded processors.

Hydrium is named after the fictitious gas from Kenneth Oppel's novel *Airborn,* which is lighter than even Hydrogen.
