.PHONY: bootstrap test-host firmware release-zip
bootstrap:
	python3 tools/bootstrap.py
test-host:
	ASAN_OPTIONS=detect_leaks=0 python3 tools/test_host.py
firmware:
	python3 tools/build_firmware.py --stack-check

release-zip:
	python3 tools/bootstrap.py --check
	$(MAKE) firmware
	python3 tools/make_release_zip.py

# These targets are available in the local research workspace.
ifneq ($(wildcard tools/run_prototype.py),)
.PHONY: test
test:
	python3 tools/run_prototype.py
endif

ifneq ($(wildcard tools/firmware_all_tests.py),)
.PHONY: test-firmware
test-firmware: firmware
	python3 tools/firmware_all_tests.py
endif
