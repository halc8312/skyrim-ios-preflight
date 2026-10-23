"""Pure selection of the public minimal-audio diagnostic's installed Simulator."""

DEFAULT_VERSION = "26.5"
DEVICE_NAME = "iPhone 17 Pro Max"
DEVICE_TYPE = "com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max"
RUNTIME_PREFIX = "com.apple.CoreSimulator.SimRuntime.iOS-"
RUNTIME_IDENTIFIERS = {
    "26.3": RUNTIME_PREFIX + "26-3",
    "26.4": RUNTIME_PREFIX + "26-4",
    "26.5": RUNTIME_PREFIX + "26-5",
    # The observed public Xcode 26.4.1 image reports this exact pair.
    "26.4.1": RUNTIME_PREFIX + "26-4",
}
SUPPORTED_VERSIONS = tuple(RUNTIME_IDENTIFIERS)


def runtime_identifier(version=DEFAULT_VERSION):
    if type(version) is not str or version not in SUPPORTED_VERSIONS:
        raise ValueError("unsupported public audio Simulator version")
    return RUNTIME_IDENTIFIERS[version]


def select_simulator(runtimes, device_types, version=DEFAULT_VERSION):
    expected_runtime = runtime_identifier(version)
    for rows in (runtimes, device_types):
        if type(rows) is not list or any(type(row) is not dict for row in rows):
            raise ValueError("invalid installed Simulator listing")
    matches = [row for row in runtimes if row.get("isAvailable") is True
               and row.get("version") == version and type(row.get("identifier")) is str
               and row["identifier"].startswith(RUNTIME_PREFIX)]
    if len(matches) != 1 or matches[0]["identifier"] != expected_runtime:
        raise ValueError("required available iOS runtime missing, crossed or ambiguous")
    devices = [row for row in device_types if row.get("name") == DEVICE_NAME]
    if len(devices) != 1 or devices[0].get("identifier") != DEVICE_TYPE:
        raise ValueError("required iPhone 17 Pro Max device type missing or ambiguous")
    major, minor = map(int, version.split(".")[:2])
    return dict(schema=1, runtime_version=version, ios_major=major, ios_minor=minor,
                runtime_identifier=expected_runtime, device_type_name=DEVICE_NAME,
                device_type_identifier=DEVICE_TYPE)
