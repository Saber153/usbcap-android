# ProGuard / R8 规则
-keep class com.usbcap.viewer.NativeRenderer { *; }
-keep class com.usbcap.viewer.MainActivity { *; }

# Native方法不混淆
-keepclasseswithmembernames class * {
    native <methods>;
}
