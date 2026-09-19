/**
 * @file        system/auto_updater_android.cpp
 * @brief       Android AutoUpdater::ApplyAndRestart: hands the staged APK to
 *              the system package installer through a FileProvider URI.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/auto_updater.h>

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <rex/logging.h>

namespace rex::system {

namespace {

// Same idea as the helper in core/net/http_android.cpp: local refs pile up
// and every call here can throw, so scope refs and clear before returning.
class LocalFrame {
 public:
  explicit LocalFrame(JNIEnv* env) : env_(env) { env_->PushLocalFrame(32); }
  ~LocalFrame() { env_->PopLocalFrame(nullptr); }

 private:
  JNIEnv* env_;
};

bool Failed(JNIEnv* env, const char* step) {
  if (!env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  REXSYS_WARN("AutoUpdater: {} threw", step);
  return true;
}

}  // namespace

bool AutoUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& /*executable_path*/) {
  auto apk = StagedApkPath(install_root);
  if (apk.empty()) {
    return false;
  }
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    REXSYS_WARN("AutoUpdater: no JNI environment or activity");
    return false;
  }
  LocalFrame frame(env);

  // App classes (androidx FileProvider) are not visible to FindClass from a
  // native thread, whose class loader is the system one; go through the
  // activity's loader instead.
  jclass activity_cls = env->GetObjectClass(activity);
  jobject loader = env->CallObjectMethod(
      activity, env->GetMethodID(activity_cls, "getClassLoader", "()Ljava/lang/ClassLoader;"));
  jclass loader_cls = env->GetObjectClass(loader);
  jmethodID load_class =
      env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  auto provider_cls = static_cast<jclass>(env->CallObjectMethod(
      loader, load_class, env->NewStringUTF("androidx.core.content.FileProvider")));
  if (Failed(env, "loading androidx.core.content.FileProvider") || !provider_cls) {
    return false;
  }

  auto package = static_cast<jstring>(env->CallObjectMethod(
      activity, env->GetMethodID(activity_cls, "getPackageName", "()Ljava/lang/String;")));
  const char* package_utf = env->GetStringUTFChars(package, nullptr);
  std::string authority = std::string(package_utf) + ".fileprovider";
  env->ReleaseStringUTFChars(package, package_utf);

  jclass file_cls = env->FindClass("java/io/File");
  jobject file =
      env->NewObject(file_cls, env->GetMethodID(file_cls, "<init>", "(Ljava/lang/String;)V"),
                     env->NewStringUTF(apk.string().c_str()));
  jobject uri = env->CallStaticObjectMethod(
      provider_cls,
      env->GetStaticMethodID(
          provider_cls, "getUriForFile",
          "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;"),
      activity, env->NewStringUTF(authority.c_str()), file);
  if (Failed(env, "FileProvider.getUriForFile") || !uri) {
    return false;
  }

  jclass intent_cls = env->FindClass("android/content/Intent");
  jobject intent =
      env->NewObject(intent_cls, env->GetMethodID(intent_cls, "<init>", "(Ljava/lang/String;)V"),
                     env->NewStringUTF("android.intent.action.VIEW"));
  env->CallObjectMethod(
      intent,
      env->GetMethodID(intent_cls, "setDataAndType",
                       "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;"),
      uri, env->NewStringUTF("application/vnd.android.package-archive"));
  constexpr jint kFlagGrantReadUriPermission = 0x00000001;
  constexpr jint kFlagActivityNewTask = 0x10000000;
  env->CallObjectMethod(intent,
                        env->GetMethodID(intent_cls, "addFlags", "(I)Landroid/content/Intent;"),
                        kFlagGrantReadUriPermission | kFlagActivityNewTask);
  env->CallVoidMethod(
      activity, env->GetMethodID(activity_cls, "startActivity", "(Landroid/content/Intent;)V"),
      intent);
  if (Failed(env, "startActivity")) {
    return false;
  }
  REXSYS_INFO("AutoUpdater: handed {} to the package installer", apk.string());
  return true;
}

}  // namespace rex::system
