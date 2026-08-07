$(TARGET): $(OFILES)
	@$(CXX) $(OFILES) -o "$@" $(LDFLAGS)	
	@if test -z "$(DEBUG)" && test -z "$(SANITIZE)"; then \
		$(STRIP) "$@"; \
	fi
	@-mv -f $(TARGET) "$(BUILD_DIR)/$(TARGET)"

build: $(TARGET)

# Per-component objects for sources outside the component directory - see the
# EXT_CFILES comment in config.mk. These match ahead of the generic rule below,
# which could not build them anyway: there is no ../common/utils/<target>-str.c
# for it to match against.
../common/utils/$(TARGET)-%.o: ../common/utils/%.c
	@$(ECHO) $(PRINT_BUILD)
	@$(ECHO) $(COMPILE_CC_OUT)

../../include/cjson/$(TARGET)-%.o: ../../include/cjson/%.c
	@$(ECHO) $(PRINT_BUILD)
	@$(ECHO) $(COMPILE_CC_OUT)

%.o: %.c
	@$(ECHO) $(PRINT_BUILD)
	@$(ECHO) $(COMPILE_CC_OUT)

%.o: %.cpp
	@$(ECHO) $(PRINT_BUILD)
	@$(ECHO) $(COMPILE_CXX_OUT)

clean:
	@$(ECHO) $(PRINT_RECIPE)
	@rm -f $(TARGET) $(OFILES)

install:
	@echo "do nothing for install"

dev: clean
	@$(MAKE_DEV)

asan: clean
	@$(MAKE_ASAN)