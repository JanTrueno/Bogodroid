#include "baron/baron.h"
#include "android.h"
#include "javac.h"
#include "limbo.h"

void InitJNIBinding(FakeJni::Jvm *vm)
{
    // Pre-NEO this listed every class by hand; the shared helpers now register
    // the full java.* / android.* surface (javastubs/javac.cpp,
    // javastubs/android_descriptors.cpp), so only the game's own classes are
    // left here.
    InitJNIJavaClasses(vm);
    InitJNIAndroidClasses(vm);

    vm->registerClass<jnivm::com::playdead::limbo::LimboActivity>();
    vm->registerClass<jnivm::com::playdead::limbo::LimboAgeSignals>();

    HookStringExtensions(vm);
    HookClassExtensions(vm);
    HookObjectExtensions(vm);
}
