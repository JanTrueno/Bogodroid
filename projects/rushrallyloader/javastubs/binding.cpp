#include "baron/baron.h"
#include "android.h"
#include "javac.h"
#include "rushrally.h"

void InitJNIBinding(FakeJni::Jvm *vm)
{
    InitJNIJavaClasses(vm);
    InitJNIAndroidClasses(vm);

    vm->registerClass<jnivm::brownmonster::app::game::rushrally3::RushRally3Activity>();

    HookStringExtensions(vm);
    HookClassExtensions(vm);
    HookObjectExtensions(vm);
}
