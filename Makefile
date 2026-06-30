.PHONY: all build package verify clean

all: build

build:
	./scripts/build.sh

package:
	./scripts/package_release.sh

verify:
	./scripts/verify.sh

clean:
	rm -rf build dist
