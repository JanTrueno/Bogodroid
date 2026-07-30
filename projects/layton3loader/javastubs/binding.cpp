#include "baron/baron.h"
#include "android.h"
#include "javac.h"
#include "layton3.h"

void InitJNIBinding(FakeJni::Jvm *vm)
{
    InitJNIJavaClasses(vm);
    InitJNIAndroidClasses(vm);

    vm->registerClass<jnivm::com::Level5::LT3R::MainActivity>();

    HookStringExtensions(vm);
    HookClassExtensions(vm);
    HookObjectExtensions(vm);
}
