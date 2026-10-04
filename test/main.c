#include <dlfcn.h>
#include <stdio.h>
#include <emscripten.h>

/* Loads the API tests the way PHP loads a dynamic extension. */
EMSCRIPTEN_KEEPALIVE int start(int port)
{
	void *handle = dlopen("/api.so", RTLD_NOW);
	if(!handle)
	{
		printf("Bail out! dlopen: %s\n", dlerror());
		return 1;
	}
	int (*run_tests)(int) = (int (*)(int))dlsym(handle, "run_tests");
	int failures = run_tests(port);
	fflush(stdout);
	return failures;
}

int main(void)
{
	return 0;
}
