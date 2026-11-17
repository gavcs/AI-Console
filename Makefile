# Makefile — AI Console (Qt6 via Homebrew, macOS)
# Sources live in src/, headers in include/.

# --- Qt location & host tools (queried from Homebrew's qmake) ---
QT_PREFIX  := $(shell brew --prefix qt)
QMAKE      := $(QT_PREFIX)/bin/qmake
QT_HEADERS := $(shell $(QMAKE) -query QT_INSTALL_HEADERS)
QT_LIBS    := $(shell $(QMAKE) -query QT_INSTALL_LIBS)
QT_LIBEXEC := $(shell $(QMAKE) -query QT_INSTALL_LIBEXECS)
MOC        := $(QT_LIBEXEC)/moc

# --- Project layout ---
SRC_DIR := src
INC_DIR := include
OBJ_DIR := obj
MOC_DIR := moc
BIN_DIR := bin
TARGET  := $(BIN_DIR)/ai_console

# --- Toolchain flags ---
CXX      := clang++
CXXFLAGS := -std=c++17 -fPIC -Wall \
            -I. -I$(INC_DIR) \
            -F$(QT_LIBS) \
            -I$(QT_HEADERS) \
            -I$(QT_HEADERS)/QtCore \
            -I$(QT_HEADERS)/QtGui \
            -I$(QT_HEADERS)/QtWidgets \
            -I$(QT_HEADERS)/QtNetwork
# Write a .d file next to each object listing the headers it included, so that
# editing a header rebuilds everything that uses it. Without this, objects
# built against different versions of a header get linked together and the app
# crashes at runtime.
DEPFLAGS := -MMD -MP
LDFLAGS  := -F$(QT_LIBS) -Wl,-rpath,$(QT_LIBS) \
            -framework QtWidgets -framework QtGui -framework QtNetwork -framework QtCore

# --- Sources / objects ---
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

# Headers that contain Q_OBJECT need moc. Detected automatically.
MOC_HDRS := $(shell grep -l Q_OBJECT $(INC_DIR)/*.h)
MOC_GEN  := $(patsubst $(INC_DIR)/%.h,$(MOC_DIR)/moc_%.cpp,$(MOC_HDRS))
MOC_OBJS := $(patsubst $(MOC_DIR)/%.cpp,$(MOC_DIR)/%.o,$(MOC_GEN))

# --- Rules ---
all: $(TARGET)

$(TARGET): $(OBJS) $(MOC_OBJS) | $(BIN_DIR)
	$(CXX) $(OBJS) $(MOC_OBJS) $(LDFLAGS) -o $@

# Compile a normal source from src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Generate a moc source from a Q_OBJECT header in include/
$(MOC_DIR)/moc_%.cpp: $(INC_DIR)/%.h | $(MOC_DIR)
	$(MOC) $< -o $@

# Compile a generated moc source
$(MOC_DIR)/%.o: $(MOC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR) $(MOC_DIR) $(BIN_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

# --- Tests ---------------------------------------------------------------
# A headless driver (tests/chat_driver.cpp) linked against the app's own
# objects, minus main.o. It drives ChatPage under QT_QPA_PLATFORM=offscreen.
TEST_BIN         := $(BIN_DIR)/chat_driver
APP_OBJS_NO_MAIN := $(filter-out $(OBJ_DIR)/main.o,$(OBJS)) $(MOC_OBJS)

SESSION_TEST_BIN := $(BIN_DIR)/session_driver

$(TEST_BIN) $(SESSION_TEST_BIN): $(BIN_DIR)/%: tests/%.cpp $(APP_OBJS_NO_MAIN) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< $(APP_OBJS_NO_MAIN) $(LDFLAGS) -o $@

test: $(TEST_BIN) $(SESSION_TEST_BIN)
	bash tests/run_tests.sh $(TEST_BIN)

clean:
	rm -rf $(OBJ_DIR) $(MOC_DIR) $(BIN_DIR)

# Header dependencies recorded by DEPFLAGS (absent on the first build).
-include $(OBJS:.o=.d) $(MOC_OBJS:.o=.d)

# A change to the build flags invalidates every object.
$(OBJS) $(MOC_OBJS): Makefile

# Keep generated moc sources so they aren't regenerated on every build.
.SECONDARY: $(MOC_GEN)

.PHONY: all run clean test
