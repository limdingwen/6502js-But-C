# If you want release builds, use XCode.
xcodebuild -project mac6502/mac6502.xcodeproj -scheme mac6502 build SYMROOT="$(pwd)"
