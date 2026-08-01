#include "baron/baron.h"
#include "android.h"
#include "javac.h"
#include "cocos2dx.h"
#include "fmod.h"
#include "robtop.h"

void InitJNIBinding(FakeJni::Jvm *vm)
{
    InitJNIJavaClasses(vm);
    InitJNIAndroidClasses(vm);

    vm->registerClass<jnivm::org::cocos2dx::lib::Cocos2dxHelper>();
    vm->registerClass<jnivm::org::cocos2dx::lib::Cocos2dxRenderer>();
    vm->registerClass<jnivm::org::cocos2dx::lib::Cocos2dxBitmap>();
    vm->registerClass<jnivm::org::fmod::FMOD>();
    vm->registerClass<jnivm::org::fmod::AudioDevice>();
    vm->registerClass<jnivm::com::customRobTop::base::BaseRobTopActivity>();

    // The game polls android.os.SystemClock.uptimeMillis() for timing; the
    // shared javastubs provide the same helper as a free function.
    {
        FakeJni::LocalFrame frame(*vm);
        auto env = jnivm::ENV::FromJNIEnv(&frame.getJniEnv());
        auto sysclock = env->GetClass("android/os/SystemClock");
        sysclock->Hook(env, "uptimeMillis",
            []() -> jlong { return (jlong)jnivm::android::os::uptimeMillis(); });
    }

    HookStringExtensions(vm);
    HookClassExtensions(vm);
    HookObjectExtensions(vm);
}
