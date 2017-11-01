/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AST.h"
#include "Coordinator.h"
#include "Scope.h"

#include <hidl-hash/Hash.h>
#include <hidl-util/Formatter.h>
#include <hidl-util/FQName.h>
#include <hidl-util/StringHelper.h>
#include <android-base/logging.h>
#include <set>
#include <stdio.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace android;

struct OutputHandler {
    std::string mKey;
    std::string mDescription;
    enum OutputMode {
        NEEDS_DIR,
        NEEDS_FILE,
        NEEDS_SRC, // for changes inside the source tree itself
        NOT_NEEDED
    } mOutputMode;

    const std::string& name() { return mKey; }
    const std::string& description() { return mDescription; }

    using ValidationFunction = std::function<bool(const FQName &, const std::string &language)>;
    using GenerationFunction = std::function<status_t(const FQName &fqName,
                                                      const char *hidl_gen,
                                                      Coordinator *coordinator,
                                                      const std::string &outputDir)>;

    ValidationFunction validate;
    GenerationFunction generate;
};

static bool generateForTest = false;

static status_t generateSourcesForFile(
        const FQName &fqName,
        const char *,
        Coordinator *coordinator,
        const std::string &outputDir,
        const std::string &lang) {
    CHECK(fqName.isFullyQualified());

    AST *ast;
    std::string limitToType;

    if (fqName.name().find("types.") == 0) {
        CHECK(lang == "java");  // Already verified in validate().

        limitToType = fqName.name().substr(strlen("types."));

        FQName typesName = fqName.getTypesForPackage();
        ast = coordinator->parse(typesName);
    } else {
        ast = coordinator->parse(fqName);
    }

    if (ast == NULL) {
        fprintf(stderr,
                "ERROR: Could not parse %s. Aborting.\n",
                fqName.string().c_str());

        return UNKNOWN_ERROR;
    }

    if (lang == "check") {
        return OK; // only parsing, not generating
    }
    if (lang == "c++") {
        return ast->generateCpp(outputDir);
    }
    if (lang == "c++-headers") {
        return ast->generateCppHeaders(outputDir);
    }
    if (lang == "c++-sources") {
        return ast->generateCppSources(outputDir);
    }
    if (lang == "c++-impl") {
        return ast->generateCppImpl(outputDir);
    }
    if (lang == "c++-impl-headers") {
        return ast->generateCppImplHeader(outputDir);
    }
    if (lang == "c++-impl-sources") {
        return ast->generateCppImplSource(outputDir);
    }
    if (lang == "c++-adapter") {
        return ast->generateCppAdapter(outputDir);
    }
    if (lang == "c++-adapter-headers") {
        return ast->generateCppAdapterHeader(outputDir);
    }
    if (lang == "c++-adapter-sources") {
        return ast->generateCppAdapterSource(outputDir);
    }
    if (lang == "java") {
        return ast->generateJava(outputDir, limitToType);
    }
    if (lang == "vts") {
        return ast->generateVts(outputDir);
    }
    // Unknown language.
    return UNKNOWN_ERROR;
}

static status_t generateSourcesForPackage(
        const FQName &packageFQName,
        const char *hidl_gen,
        Coordinator *coordinator,
        const std::string &outputDir,
        const std::string &lang) {
    CHECK(packageFQName.isValid() &&
        !packageFQName.isFullyQualified() &&
        packageFQName.name().empty());

    std::vector<FQName> packageInterfaces;

    status_t err =
        coordinator->appendPackageInterfacesToVector(packageFQName,
                                                     &packageInterfaces);

    if (err != OK) {
        return err;
    }

    for (const auto &fqName : packageInterfaces) {
        err = generateSourcesForFile(
                fqName, hidl_gen, coordinator, outputDir, lang);
        if (err != OK) {
            return err;
        }
    }

    return OK;
}

OutputHandler::GenerationFunction generationFunctionForFileOrPackage(const std::string &language) {
    return [language](const FQName &fqName,
              const char *hidl_gen, Coordinator *coordinator,
              const std::string &outputDir) -> status_t {
        if (fqName.isFullyQualified()) {
                    return generateSourcesForFile(fqName,
                                                  hidl_gen,
                                                  coordinator,
                                                  outputDir,
                                                  language);
        } else {
                    return generateSourcesForPackage(fqName,
                                                     hidl_gen,
                                                     coordinator,
                                                     outputDir,
                                                     language);
        }
    };
}

static std::string makeLibraryName(const FQName &packageFQName) {
    return packageFQName.string();
}
static std::string makeHalFilegroupName(const FQName& packageFQName) {
    return packageFQName.string() + "_hal";
}

static std::string makeJavaLibraryName(const FQName &packageFQName) {
    std::string out;
    out = packageFQName.package();
    out += "-V";
    out += packageFQName.version();
    out += "-java";
    return out;
}

static void generatePackagePathsSection(
        Formatter &out,
        Coordinator *coordinator,
        const FQName &packageFQName,
        const std::set<FQName> &importedPackages,
        bool forMakefiles = false) {
    std::set<std::string> options{};
    for (const auto &interface : importedPackages) {
        options.insert(coordinator->getPackageRootOption(interface));
    }
    options.insert(coordinator->getPackageRootOption(packageFQName));
    options.insert(coordinator->getPackageRootOption(gIBaseFqName));
    for (const auto &option : options) {
        out << "-r"
            << option
            << " ";
        if (forMakefiles) {
            out << "\\\n";
        }
    }
}

static status_t isPackageJavaCompatible(
        const FQName &packageFQName,
        Coordinator *coordinator,
        bool *compatible) {
    std::vector<FQName> todo;
    status_t err =
        coordinator->appendPackageInterfacesToVector(packageFQName, &todo);

    if (err != OK) {
        return err;
    }

    std::set<FQName> seen;
    for (const auto &iface : todo) {
        seen.insert(iface);
    }

    // Form the transitive closure of all imported interfaces (and types.hal-s)
    // If any one of them is not java compatible, this package isn't either.
    while (!todo.empty()) {
        const FQName fqName = todo.back();
        todo.pop_back();

        AST *ast = coordinator->parse(fqName);

        if (ast == nullptr) {
            return UNKNOWN_ERROR;
        }

        if (!ast->isJavaCompatible()) {
            *compatible = false;
            return OK;
        }

        std::set<FQName> importedPackages;
        ast->getImportedPackages(&importedPackages);

        for (const auto &package : importedPackages) {
            std::vector<FQName> packageInterfaces;
            status_t err = coordinator->appendPackageInterfacesToVector(
                    package, &packageInterfaces);

            if (err != OK) {
                return err;
            }

            for (const auto &iface : packageInterfaces) {
                if (seen.find(iface) != seen.end()) {
                    continue;
                }

                todo.push_back(iface);
                seen.insert(iface);
            }
        }
    }

    *compatible = true;
    return OK;
}

static bool packageNeedsJavaCode(
        const std::vector<FQName> &packageInterfaces, AST *typesAST) {
    if (packageInterfaces.size() == 0) {
        return false;
    }

    // If there is more than just a types.hal file to this package we'll
    // definitely need to generate Java code.
    if (packageInterfaces.size() > 1
            || packageInterfaces[0].name() != "types") {
        return true;
    }

    CHECK(typesAST != nullptr);

    // We'll have to generate Java code if types.hal contains any non-typedef
    // type declarations.

    Scope* rootScope = typesAST->getRootScope();
    std::vector<NamedType *> subTypes = rootScope->getSubTypes();

    for (const auto &subType : subTypes) {
        if (!subType->isTypeDef()) {
            return true;
        }
    }

    return false;
}

bool validateIsPackage(
        const FQName &fqName, const std::string & /* language */) {
    if (fqName.package().empty()) {
        fprintf(stderr, "ERROR: Expecting package name\n");
        return false;
    }

    if (fqName.version().empty()) {
        fprintf(stderr, "ERROR: Expecting package version\n");
        return false;
    }

    if (!fqName.name().empty()) {
        fprintf(stderr,
                "ERROR: Expecting only package name and version.\n");
        return false;
    }

    return true;
}

bool isHidlTransportPackage(const FQName& fqName) {
    return fqName.package() == gIBasePackageFqName.string() ||
           fqName.package() == gIManagerPackageFqName.string();
}

bool isSystemProcessSupportedPackage(const FQName& fqName) {
    // Technically, so is hidl IBase + IServiceManager, but
    // these are part of libhidltransport.
    return fqName.string() == "android.hardware.graphics.allocator@2.0" ||
           fqName.string() == "android.hardware.graphics.common@1.0" ||
           fqName.string() == "android.hardware.graphics.mapper@2.0" ||
           fqName.string() == "android.hardware.graphics.mapper@2.1" ||
           fqName.string() == "android.hardware.renderscript@1.0" ||
           fqName.string() == "android.hidl.memory@1.0";
}

bool isSystemPackage(const FQName &package) {
    return package.inPackage("android.hidl") ||
           package.inPackage("android.system") ||
           package.inPackage("android.frameworks") ||
           package.inPackage("android.hardware");
}

static void generateAndroidBpGenSection(
    Formatter& out,
    const FQName& packageFQName,
    const char* hidl_gen,
    Coordinator* coordinator,
    const std::string& halFilegroupName,
    const std::string& genName,
    const char* language,
    const std::vector<FQName>& packageInterfaces,
    const std::set<FQName>& importedPackages,
    const std::function<void(Formatter&, const FQName)>& outputFn) {
    out << "genrule {\n";
    out.indent();
    out << "name: \"" << genName << "\",\n"
        << "tools: [\"" << hidl_gen << "\"],\n";

    out << "cmd: \"$(location " << hidl_gen << ") -o $(genDir)"
        << " -L" << language << " ";

    generatePackagePathsSection(out, coordinator, packageFQName, importedPackages);

    out << packageFQName.string() << "\",\n";

    out << "srcs: [\n";
    out.indent();
    out << "\":" << halFilegroupName << "\",\n";
    out.unindent();
    out << "],\n";

    out << "out: [\n";
    out.indent();
    for (const auto &fqName : packageInterfaces) {
        outputFn(out, fqName);
    }
    out.unindent();
    out << "],\n";

    out.unindent();
    out << "}\n\n";
}

static void generateAndroidBpDependencyList(
        Formatter &out,
        const std::set<FQName> &importedPackagesHierarchy) {
    for (const auto &importedPackage : importedPackagesHierarchy) {
        if (isHidlTransportPackage(importedPackage)) {
            continue;
        }

        out << "\"" << makeLibraryName(importedPackage);
        out << "\",\n";
    }
}

enum class LibraryLocation {
    // NONE,
    VENDOR,
    VENDOR_AVAILABLE,
    VNDK,
};

static void generateAndroidBpCppLibSection(
        Formatter &out,
        LibraryLocation libraryLocation,
        const FQName &packageFQName,
        const std::string &libraryName,
        const std::string &genSourceName,
        const std::string &genHeaderName,
        std::function<void(void)> generateDependencies) {

    // C++ library definition
    out << "cc_library {\n";
    out.indent();
    out << "name: \"" << libraryName << "\",\n"
        << "defaults: [\"hidl-module-defaults\"],\n"
        << "generated_sources: [\"" << genSourceName << "\"],\n"
        << "generated_headers: [\"" << genHeaderName << "\"],\n"
        << "export_generated_headers: [\"" << genHeaderName << "\"],\n";

    switch (libraryLocation) {
    case LibraryLocation::VENDOR: {
        out << "vendor: true,\n";
        break;
    }
    case LibraryLocation::VENDOR_AVAILABLE: {
        out << "vendor_available: true,\n";
        break;
    }
    case LibraryLocation::VNDK: {
        out << "vendor_available: true,\n";
        out << "vndk: ";
        out.block([&]() {
            out << "enabled: true,\n";
            if (isSystemProcessSupportedPackage(packageFQName)) {
                out << "support_system_process: true,\n";
            }
        }) << ",\n";
        break;
    }
    default: {
        CHECK(false) << "Invalid library type specified in " << __func__;
    }
    }

    out << "shared_libs: [\n";

    out.indent();
    out << "\"libhidlbase\",\n"
        << "\"libhidltransport\",\n"
        << "\"libhwbinder\",\n"
        << "\"liblog\",\n"
        << "\"libutils\",\n"
        << "\"libcutils\",\n";
    generateDependencies();

    out.unindent();

    out << "],\n";

    out << "export_shared_lib_headers: [\n";
    out.indent();
    out << "\"libhidlbase\",\n"
        << "\"libhidltransport\",\n"
        << "\"libhwbinder\",\n"
        << "\"libutils\",\n";
    generateDependencies();
    out.unindent();
    out << "],\n";
    out.unindent();

    out << "}\n";
}

static status_t generateAdapterMainSource(
        const FQName & packageFQName,
        const char* /* hidl_gen */,
        Coordinator* coordinator,
        const std::string &outputPath) {
    Formatter out = coordinator->getFormatter(outputPath, packageFQName,
                                              Coordinator::Location::DIRECT, "main.cpp");

    if (!out.isValid()) {
        return UNKNOWN_ERROR;
    }

    std::vector<FQName> packageInterfaces;
    status_t err =
        coordinator->appendPackageInterfacesToVector(packageFQName,
                                                     &packageInterfaces);
    if (err != OK) {
        return err;
    }

    out << "#include <hidladapter/HidlBinderAdapter.h>\n";

    for (auto &interface : packageInterfaces) {
        if (interface.name() == "types") {
            continue;
        }
        AST::generateCppPackageInclude(out, interface, interface.getInterfaceAdapterName());
    }

    out << "int main(int argc, char** argv) ";
    out.block([&] {
        out << "return ::android::hardware::adapterMain<\n";
        out.indent();
        for (auto &interface : packageInterfaces) {
            if (interface.name() == "types") {
                continue;
            }
            out << interface.getInterfaceAdapterFqName().cppName();

            if (&interface != &packageInterfaces.back()) {
                out << ",\n";
            }
        }
        out << ">(\"" << packageFQName.string() << "\", argc, argv);\n";
        out.unindent();
    }).endl();
    return OK;
}

static void generateAndroidBpDefinitionLibsForPackage(
    Formatter& out, const FQName& packageFQName, const char* hidl_gen, Coordinator* coordinator,
    const std::vector<FQName>& packageInterfaces,
    const std::set<FQName>& importedPackagesHierarchy) {
    const std::string libraryName = makeLibraryName(packageFQName);
    const std::string halFilegroupName = makeHalFilegroupName(packageFQName);
    const std::string genSourceName = libraryName + "_genc++";
    const std::string genHeaderName = libraryName + "_genc++_headers";
    const std::string pathPrefix = coordinator->getFilepath("" /* outputPath */, packageFQName,
                                                            Coordinator::Location::GEN_OUTPUT);

    // Rule to generate the C++ source files
    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genSourceName,
            "c++-sources",
            packageInterfaces,
            importedPackagesHierarchy,
            [&pathPrefix](Formatter &out, const FQName &fqName) {
                if (fqName.name() == "types") {
                    out << "\"" << pathPrefix << "types.cpp\",\n";
                } else {
                    out << "\"" << pathPrefix << fqName.name().substr(1) << "All.cpp\",\n";
                }
            });

    // Rule to generate the C++ header files
    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genHeaderName,
            "c++-headers",
            packageInterfaces,
            importedPackagesHierarchy,
            [&pathPrefix](Formatter &out, const FQName &fqName) {
                out << "\"" << pathPrefix << fqName.name() << ".h\",\n";
                if (fqName.name() != "types") {
                    out << "\"" << pathPrefix << fqName.getInterfaceHwName() << ".h\",\n";
                    out << "\"" << pathPrefix << fqName.getInterfaceStubName() << ".h\",\n";
                    out << "\"" << pathPrefix << fqName.getInterfaceProxyName() << ".h\",\n";
                    out << "\"" << pathPrefix << fqName.getInterfacePassthroughName() << ".h\",\n";
                } else {
                    out << "\"" << pathPrefix << "hwtypes.h\",\n";
                }
            });

    if (isHidlTransportPackage(packageFQName)) {
        out << "// " << packageFQName.string() << " is exported from libhidltransport\n";
    } else {
        bool isVndk = !generateForTest && isSystemPackage(packageFQName);

        generateAndroidBpCppLibSection(
            out,
            (isVndk ? LibraryLocation::VNDK : LibraryLocation::VENDOR_AVAILABLE),
            packageFQName,
            libraryName,
            genSourceName,
            genHeaderName,
            [&]() {
                generateAndroidBpDependencyList(out, importedPackagesHierarchy);
            });
    }

    out.endl();
}

static void generateAndroidBpJavaLibsForPackage(
    Formatter& out, const FQName& packageFQName, const char* hidl_gen, Coordinator* coordinator,
    const std::vector<FQName>& packageInterfaces,
    const std::set<FQName>& importedPackagesHierarchy, AST *typesAST) {

    const std::string libraryName = makeJavaLibraryName(packageFQName);
    const std::string halFilegroupName = makeHalFilegroupName(packageFQName);
    const std::string genJavaName = libraryName + "_gen_java";

    const std::string pathPrefix = coordinator->getFilepath("" /* outputPath */, packageFQName,
                                                            Coordinator::Location::GEN_SANITIZED);

    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genJavaName,
            "java",
            packageInterfaces,
            importedPackagesHierarchy,
            [&pathPrefix, &typesAST](Formatter &out, const FQName &fqName) {
                if (fqName.name() != "types") {
                    out << "\"" << pathPrefix << fqName.name() << ".java\",\n";
                    return;
                }

                CHECK(typesAST != nullptr);

                std::vector<NamedType *> subTypes = typesAST->getRootScope()->getSubTypes();
                std::sort(
                        subTypes.begin(),
                        subTypes.end(),
                        [](const NamedType *a, const NamedType *b) -> bool {
                            return a->fqName() < b->fqName();
                        });

                for (const auto &type : subTypes) {
                    if (type->isTypeDef()) {
                        continue;
                    }

                    out << "\"" << pathPrefix << type->localName() << ".java\",\n";
                }
            });

    out << "java_library {\n";
    out.indent([&] {
        out << "name: \"" << libraryName << "\",\n";
        out << "no_framework_libs: true,\n";
        out << "defaults: [\"hidl-java-module-defaults\"],\n";
        out << "srcs: [\":" << genJavaName << "\"],\n";
        out << "libs: [\n";
        out.indent([&] {
            out << "\"hwbinder\",\n";
            for (const auto &importedPackage : importedPackagesHierarchy) {
                out << "\"" << makeJavaLibraryName(importedPackage) << "\",\n";
            }
        });
        out << "]\n";
    });
    out << "}\n\n";
}

static void generateAndroidBpJavaExportsForPackage(
    Formatter& out, const FQName& packageFQName, const char* hidl_gen, Coordinator* coordinator,
    const std::vector<FQName>& packageInterfaces,
    const std::set<FQName>& importedPackagesHierarchy,
    const std::vector<const Type *>& exportedTypes) {

    const std::string libraryName = makeJavaLibraryName(packageFQName);
    const std::string halFilegroupName = makeHalFilegroupName(packageFQName);
    const std::string genJavaName = libraryName + "_gen_java";

    CHECK(!exportedTypes.empty());

    const std::string pathPrefix = coordinator->getFilepath("" /* outputPath */, packageFQName,
                                                            Coordinator::Location::GEN_SANITIZED);

    const std::string constantsLibraryName = libraryName + "-constants";
    const std::string genConstantsName = constantsLibraryName + "_gen_java";

    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genConstantsName,
            "java-constants",
            packageInterfaces,
            importedPackagesHierarchy,
            [&pathPrefix](Formatter &out, const FQName &/* fqName */) {
                static bool once = false;
                if (!once) {
                    out << "\"" << pathPrefix << "Constants.java\",\n";
                    once = true;
                }
            });

    out << "java_library {\n";
    out.indent();
    out << "name: \"" << constantsLibraryName << "\",\n";
    out << "no_framework_libs: true,\n";
    out << "defaults: [\"hidl-java-module-defaults\"],\n";
    out << "srcs: [\":" << genConstantsName << "\"],\n";
    out.unindent();
    out << "}\n";
}

static status_t generateAndroidBpAdapterLibsForPackage(
    Formatter& out, const FQName& packageFQName, const char* hidl_gen, Coordinator* coordinator,
    const std::vector<FQName>& packageInterfaces,
    const std::set<FQName>& importedPackagesHierarchy) {
    const std::string adapterName = makeLibraryName(packageFQName) + "-adapter";
    const std::string halFilegroupName = makeHalFilegroupName(packageFQName);
    const std::string genAdapterName = adapterName + "_genc++";
    const std::string adapterHelperName = adapterName + "-helper";
    const std::string genAdapterSourcesName = adapterHelperName + "_genc++";
    const std::string genAdapterHeadersName = adapterHelperName + "_genc++_headers";
    const std::string pathPrefix = coordinator->getFilepath("" /* outputPath */, packageFQName,
                                                            Coordinator::Location::GEN_OUTPUT);

    std::set<FQName> adapterPackages = importedPackagesHierarchy;
    adapterPackages.insert(packageFQName);

    out.endl();
    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genAdapterSourcesName,
            "c++-adapter-sources",
            packageInterfaces,
            adapterPackages,
            [&pathPrefix](Formatter &out, const FQName &fqName) {
                if (fqName.name() != "types") {
                    out << "\"" << pathPrefix << fqName.getInterfaceAdapterName() << ".cpp\",\n";
                }
            });
    generateAndroidBpGenSection(
            out,
            packageFQName,
            hidl_gen,
            coordinator,
            halFilegroupName,
            genAdapterHeadersName,
            "c++-adapter-headers",
            packageInterfaces,
            adapterPackages,
            [&pathPrefix](Formatter &out, const FQName &fqName) {
                if (fqName.name() != "types") {
                    out << "\"" << pathPrefix << fqName.getInterfaceAdapterName() << ".h\",\n";
                }
            });

    status_t err = OK;
    generateAndroidBpCppLibSection(
        out,
        LibraryLocation::VENDOR_AVAILABLE,
        packageFQName,
        adapterHelperName,
        genAdapterSourcesName,
        genAdapterHeadersName,
        [&]() {
            out << "\"libhidladapter\",\n";
            generateAndroidBpDependencyList(out, adapterPackages);
            for (const auto &importedPackage : importedPackagesHierarchy) {
                if (importedPackage == packageFQName) {
                    continue;
                }

                bool isTypesOnly;
                err = coordinator->isTypesOnlyPackage(importedPackage, &isTypesOnly);
                if (err != OK) {
                    return;
                }
                if (isTypesOnly) {
                    continue;
                }

                out << "\""
                    << makeLibraryName(importedPackage)
                    << "-adapter-helper"
                    << "\",\n";
            }
        });
    if (err != OK) return err;

    out.endl();

    out << "genrule {\n";
    out.indent();
    out << "name: \"" << genAdapterName << "\",\n";
    out << "tools: [\"" << hidl_gen << "\"],\n";
    out << "cmd: \"$(location " << hidl_gen << ") -o $(genDir)" << " -Lc++-adapter-main ";
    generatePackagePathsSection(out, coordinator, packageFQName, adapterPackages);
    out << packageFQName.string() << "\",\n";
    out << "out: [\"main.cpp\"]\n";
    out.unindent();
    out << "}\n\n";

    out << "cc_test {\n";
    out.indent();
    out << "name: \"" << adapterName << "\",\n";
    out << "defaults: [\"hidl-module-defaults\"],\n";
    out << "shared_libs: [\n";
    out.indent();
    out << "\"libhidladapter\",\n";
    out << "\"libhidlbase\",\n";
    out << "\"libhidltransport\",\n";
    out << "\"libutils\",\n";
    generateAndroidBpDependencyList(out, adapterPackages);
    out << "\"" << adapterHelperName << "\",\n";
    out.unindent();
    out << "],\n";
    out << "generated_sources: [\"" << genAdapterName << "\"],\n";
    out.unindent();
    out << "}\n";

    return OK;
}

static status_t generateAndroidBpForPackage(const FQName& packageFQName, const char* hidl_gen,
                                            Coordinator* coordinator,
                                            const std::string& outputPath) {
    CHECK(packageFQName.isValid() && !packageFQName.isFullyQualified() &&
          packageFQName.name().empty());

    std::vector<FQName> packageInterfaces;

    status_t err = coordinator->appendPackageInterfacesToVector(packageFQName, &packageInterfaces);

    if (err != OK) {
        return err;
    }

    std::set<FQName> importedPackagesHierarchy;
    std::vector<const Type *> exportedTypes;
    AST* typesAST = nullptr;

    for (const auto& fqName : packageInterfaces) {
        AST* ast = coordinator->parse(fqName);

        if (ast == NULL) {
            fprintf(stderr, "ERROR: Could not parse %s. Aborting.\n", fqName.string().c_str());

            return UNKNOWN_ERROR;
        }

        if (fqName.name() == "types") {
            typesAST = ast;
        }

        ast->getImportedPackagesHierarchy(&importedPackagesHierarchy);
        ast->appendToExportedTypesVector(&exportedTypes);
    }

    bool isTypesOnly;
    err = coordinator->isTypesOnlyPackage(packageFQName, &isTypesOnly);
    if (err != OK) return err;

    bool isJavaCompatible;
    err = isPackageJavaCompatible(packageFQName, coordinator, &isJavaCompatible);
    if (err != OK) return err;

    Formatter out = coordinator->getFormatter(outputPath, packageFQName,
                                              Coordinator::Location::PACKAGE_ROOT, "Android.bp");

    if (!out.isValid()) {
        return UNKNOWN_ERROR;
    }

    out << "// This file is autogenerated by hidl-gen. Do not edit manually.\n\n";

    out << "filegroup ";
    out.block([&] {
       out << "name: \"" << makeHalFilegroupName(packageFQName) << "\",\n";
       out << "srcs: [\n";
       out.indent([&] {
           for (const auto& fqName : packageInterfaces) {
               out << "\"" << fqName.name() << ".hal\",\n";
           }
       });
       out << "],\n";
   }).endl().endl();

    generateAndroidBpDefinitionLibsForPackage(out, packageFQName, hidl_gen, coordinator,
                                              packageInterfaces, importedPackagesHierarchy);

    if (packageNeedsJavaCode(packageInterfaces, typesAST)) {
        if (isJavaCompatible) {
            generateAndroidBpJavaLibsForPackage(out, packageFQName, hidl_gen, coordinator,
                                            packageInterfaces, importedPackagesHierarchy, typesAST);
        } else {
            out << "// This package is not java compatible. Not creating java target.\n\n";
        }

        if (!exportedTypes.empty()) {
            generateAndroidBpJavaExportsForPackage(out, packageFQName, hidl_gen, coordinator, packageInterfaces, importedPackagesHierarchy, exportedTypes);
        } else {
            out << "// This package does not export any types. Not creating java constants export.\n\n";
        }
    } else {
        out << "// This package has nothing to generate Java code.\n\n";
    }

    if (!isTypesOnly) {
        err = generateAndroidBpAdapterLibsForPackage(out, packageFQName, hidl_gen, coordinator,
                                                     packageInterfaces, importedPackagesHierarchy);
        if (err != OK) return err;
    } else {
        out << "// This package has no interfaces. Not creating versioning adapter.\n";
    }

    return OK;
}

static status_t generateAndroidBpImplForPackage(const FQName& packageFQName, const char*,
                                                Coordinator* coordinator,
                                                const std::string& outputPath) {
    const std::string libraryName = makeLibraryName(packageFQName) + "-impl";

    std::vector<FQName> packageInterfaces;

    status_t err =
        coordinator->appendPackageInterfacesToVector(packageFQName,
                                                     &packageInterfaces);

    if (err != OK) {
        return err;
    }

    std::set<FQName> importedPackages;

    for (const auto &fqName : packageInterfaces) {
        AST *ast = coordinator->parse(fqName);

        if (ast == NULL) {
            fprintf(stderr,
                    "ERROR: Could not parse %s. Aborting.\n",
                    fqName.string().c_str());

            return UNKNOWN_ERROR;
        }

        ast->getImportedPackages(&importedPackages);
    }

    Formatter out = coordinator->getFormatter(outputPath, packageFQName,
                                              Coordinator::Location::DIRECT, "Android.bp");

    if (!out.isValid()) {
        return UNKNOWN_ERROR;
    }

    out << "cc_library_shared {\n";
    out.indent([&] {
        out << "name: \"" << libraryName << "\",\n"
            << "relative_install_path: \"hw\",\n"
            << "proprietary: true,\n"
            << "srcs: [\n";
        out.indent([&] {
            for (const auto &fqName : packageInterfaces) {
                if (fqName.name() == "types") {
                    continue;
                }
                out << "\"" << fqName.getInterfaceBaseName() << ".cpp\",\n";
            }
        });
        out << "],\n"
            << "shared_libs: [\n";
        out.indent([&] {
            out << "\"libhidlbase\",\n"
                << "\"libhidltransport\",\n"
                << "\"libutils\",\n"
                << "\"" << makeLibraryName(packageFQName) << "\",\n";

            for (const auto &importedPackage : importedPackages) {
                if (isHidlTransportPackage(importedPackage)) {
                    continue;
                }

                out << "\"" << makeLibraryName(importedPackage) << "\",\n";
            }
        });
        out << "],\n";
    });
    out << "}\n";

    return OK;
}

bool validateForSource(
        const FQName &fqName, const std::string &language) {
    if (fqName.package().empty()) {
        fprintf(stderr, "ERROR: Expecting package name\n");
        return false;
    }

    if (fqName.version().empty()) {
        fprintf(stderr, "ERROR: Expecting package version\n");
        return false;
    }

    const std::string &name = fqName.name();
    if (!name.empty()) {
        if (name.find('.') == std::string::npos) {
            return true;
        }

        if (language != "java" || name.find("types.") != 0) {
            // When generating java sources for "types.hal", output can be
            // constrained to just one of the top-level types declared
            // by using the extended syntax
            // android.hardware.Foo@1.0::types.TopLevelTypeName.
            // In all other cases (different language, not 'types') the dot
            // notation in the name is illegal in this context.
            return false;
        }

        return true;
    }

    return true;
}

OutputHandler::GenerationFunction generateExportHeaderForPackage(bool forJava) {
    return [forJava](const FQName &packageFQName,
                     const char * /* hidl_gen */,
                     Coordinator *coordinator,
                     const std::string &outputPath) -> status_t {
        CHECK(packageFQName.isValid()
                && !packageFQName.package().empty()
                && !packageFQName.version().empty()
                && packageFQName.name().empty());

        std::vector<FQName> packageInterfaces;

        status_t err = coordinator->appendPackageInterfacesToVector(
                packageFQName, &packageInterfaces);

        if (err != OK) {
            return err;
        }

        std::vector<const Type *> exportedTypes;

        for (const auto &fqName : packageInterfaces) {
            AST *ast = coordinator->parse(fqName);

            if (ast == NULL) {
                fprintf(stderr,
                        "ERROR: Could not parse %s. Aborting.\n",
                        fqName.string().c_str());

                return UNKNOWN_ERROR;
            }

            ast->appendToExportedTypesVector(&exportedTypes);
        }

        if (exportedTypes.empty()) {
            return OK;
        }

        // C++ filename is specified in output path
        const std::string filename = forJava ? "Constants.java" : "";
        const Coordinator::Location location =
            forJava ? Coordinator::Location::GEN_SANITIZED : Coordinator::Location::DIRECT;

        Formatter out = coordinator->getFormatter(outputPath, packageFQName, location, filename);

        if (!out.isValid()) {
            return UNKNOWN_ERROR;
        }

        out << "// This file is autogenerated by hidl-gen. Do not edit manually.\n"
            << "// Source: " << packageFQName.string() << "\n"
            << "// Root: " << coordinator->getPackageRootOption(packageFQName) << "\n\n";

        std::string guard;
        if (forJava) {
            out << "package " << packageFQName.javaPackage() << ";\n\n";
            out << "public class Constants {\n";
            out.indent();
        } else {
            guard = "HIDL_GENERATED_";
            guard += StringHelper::Uppercase(packageFQName.tokenName());
            guard += "_";
            guard += "EXPORTED_CONSTANTS_H_";

            out << "#ifndef "
                << guard
                << "\n#define "
                << guard
                << "\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
        }

        for (const auto &type : exportedTypes) {
            type->emitExportedHeader(out, forJava);
        }

        if (forJava) {
            out.unindent();
            out << "}\n";
        } else {
            out << "#ifdef __cplusplus\n}\n#endif\n\n#endif  // "
                << guard
                << "\n";
        }

        return OK;
    };
}

static status_t generateHashOutput(const FQName &fqName,
        const char* /*hidl_gen*/,
        Coordinator *coordinator,
        const std::string & /*outputDir*/) {

    status_t err;
    std::vector<FQName> packageInterfaces;

    if (fqName.isFullyQualified()) {
        packageInterfaces = {fqName};
    } else {
        err = coordinator->appendPackageInterfacesToVector(
                fqName, &packageInterfaces);
        if (err != OK) {
            return err;
        }
    }

    for (const auto &currentFqName : packageInterfaces) {
        AST* ast = coordinator->parse(currentFqName, {} /* parsed */,
                                      Coordinator::Enforce::NO_HASH /* enforcement */);

        if (ast == NULL) {
            fprintf(stderr,
                    "ERROR: Could not parse %s. Aborting.\n",
                    currentFqName.string().c_str());

            return UNKNOWN_ERROR;
        }

        printf("%s %s\n",
                Hash::getHash(ast->getFilename()).hexString().c_str(),
                currentFqName.string().c_str());
    }

    return OK;
}

// clang-format off
static std::vector<OutputHandler> formats = {
    {"check",
     "Parses the interface to see if valid but doesn't write any files.",
     OutputHandler::NOT_NEEDED /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("check")
    },

    {"c++",
     "(internal) (deprecated) Generates C++ interface files for talking to HIDL interfaces.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++")
    },
    {"c++-headers",
     "(internal) Generates C++ headers for interface files for talking to HIDL interfaces.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-headers")
    },
    {"c++-sources",
     "(internal) Generates C++ sources for interface files for talking to HIDL interfaces.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-sources")
    },

    {"export-header",
     "Generates a header file from @export enumerations to help maintain legacy code.",
     OutputHandler::NEEDS_FILE /* mOutputMode */,
     validateIsPackage,
     generateExportHeaderForPackage(false /* forJava */)
    },

    {"c++-impl",
     "Generates boilerplate implementation of a hidl interface in C++ (for convenience).",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-impl")
    },
    {"c++-impl-headers",
     "c++-impl but headers only",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-impl-headers")
    },
    {"c++-impl-sources",
     "c++-impl but sources only",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-impl-sources")
    },

    {"c++-adapter",
     "Takes a x.(y+n) interface and mocks an x.y interface.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-adapter")
    },
    {"c++-adapter-headers",
     "c++-adapter but helper headers only",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-adapter-headers")
    },
    {"c++-adapter-sources",
     "c++-adapter but helper sources only",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("c++-adapter-sources")
    },
    {"c++-adapter-main",
     "c++-adapter but the adapter binary source only",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateIsPackage,
     generateAdapterMainSource,
    },

    {"java",
     "(internal) Generates Java library for talking to HIDL interfaces in Java.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("java")
    },

    {"java-constants",
     "(internal) Like export-header but for Java (always created by -Lmakefile if @export exists).",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateIsPackage,
     generateExportHeaderForPackage(true /* forJava */)
    },

    {"vts",
     "(internal) Generates vts proto files for use in vtsd.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateForSource,
     generationFunctionForFileOrPackage("vts")
    },

    {"makefile",
     "(removed) Used to generate makefiles for -Ljava and -Ljava-constants.",
     OutputHandler::NEEDS_SRC /* mOutputMode */,
     [](const FQName &, const std::string &) {
        fprintf(stderr, "ERROR: makefile output is not supported. Use -Landroidbp for all build file generation.\n");
        return false;
     },
     nullptr,
    },

    {"androidbp",
     "(internal) Generates Soong bp files for -Lc++-headers, -Lc++-sources, -Ljava, -Ljava-constants, and -Lc++-adapter.",
     OutputHandler::NEEDS_SRC /* mOutputMode */,
     validateIsPackage,
     generateAndroidBpForPackage,
    },

    {"androidbp-impl",
     "Generates boilerplate bp files for implementation created with -Lc++-impl.",
     OutputHandler::NEEDS_DIR /* mOutputMode */,
     validateIsPackage,
     generateAndroidBpImplForPackage,
    },

    {"hash",
     "Prints hashes of interface in `current.txt` format to standard out.",
     OutputHandler::NOT_NEEDED /* mOutputMode */,
     validateForSource,
     generateHashOutput,
    },
};
// clang-format on

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s [-p <root path>] -o <output path> -L <language> (-r <interface root>)+ [-t] [-v]"
            "fqname+\n",
            me);

    fprintf(stderr, "         -h: Prints this menu.\n");
    fprintf(stderr, "         -L <language>: The following options are available:\n");
    for (auto &e : formats) {
        fprintf(stderr, "            %-16s: %s\n", e.name().c_str(), e.description().c_str());
    }
    fprintf(stderr, "         -o <output path>: Location to output files.\n");
    fprintf(stderr, "         -p <root path>: Android build root, defaults to $ANDROID_BUILD_TOP or pwd.\n");
    fprintf(stderr, "         -r <package:path root>: E.g., android.hardware:hardware/interfaces.\n");
    fprintf(stderr, "         -t: generate build scripts (Android.bp) for tests.\n");
    fprintf(stderr, "         -v: verbose output (locations of touched files).\n");
}

// hidl is intentionally leaky. Turn off LeakSanitizer by default.
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

int main(int argc, char **argv) {
    const char *me = argv[0];
    if (argc == 1) {
        usage(me);
        exit(1);
    }

    OutputHandler *outputFormat = nullptr;
    Coordinator coordinator;
    std::string outputPath;

    const char *ANDROID_BUILD_TOP = getenv("ANDROID_BUILD_TOP");
    if (ANDROID_BUILD_TOP != nullptr) {
        coordinator.setRootPath(ANDROID_BUILD_TOP);
    }

    int res;
    while ((res = getopt(argc, argv, "hp:o:r:L:tv")) >= 0) {
        switch (res) {
            case 'p':
            {
                coordinator.setRootPath(optarg);
                break;
            }

            case 'v':
            {
                coordinator.setVerbose(true);
                break;
            }

            case 'o':
            {
                outputPath = optarg;
                break;
            }

            case 'r':
            {
                std::string val(optarg);
                auto index = val.find_first_of(':');
                if (index == std::string::npos) {
                    fprintf(stderr, "ERROR: -r option must contain ':': %s\n", val.c_str());
                    exit(1);
                }

                auto root = val.substr(0, index);
                auto path = val.substr(index + 1);

                std::string error;
                status_t err = coordinator.addPackagePath(root, path, &error);
                if (err != OK) {
                    fprintf(stderr, "%s\n", error.c_str());
                    exit(1);
                }

                break;
            }

            case 'L':
            {
                if (outputFormat != nullptr) {
                    fprintf(stderr,
                            "ERROR: only one -L option allowed. \"%s\" already specified.\n",
                            outputFormat->name().c_str());
                    exit(1);
                }
                for (auto &e : formats) {
                    if (e.name() == optarg) {
                        outputFormat = &e;
                        break;
                    }
                }
                if (outputFormat == nullptr) {
                    fprintf(stderr,
                            "ERROR: unrecognized -L option: \"%s\".\n",
                            optarg);
                    exit(1);
                }
                break;
            }

            case 't': {
                generateForTest = true;
                break;
            }

            case '?':
            case 'h':
            default:
            {
                usage(me);
                exit(1);
                break;
            }
        }
    }

    if (outputFormat == nullptr) {
        fprintf(stderr,
            "ERROR: no -L option provided.\n");
        exit(1);
    }

    if (generateForTest && outputFormat->name() != "androidbp") {
        fprintf(stderr, "ERROR: -t option is for -Landroidbp only.\n");
        exit(1);
    }

    argc -= optind;
    argv += optind;

    if (argc == 0) {
        fprintf(stderr, "ERROR: no fqname specified.\n");
        usage(me);
        exit(1);
    }

    // Valid options are now in argv[0] .. argv[argc - 1].

    switch (outputFormat->mOutputMode) {
        case OutputHandler::NEEDS_DIR:
        case OutputHandler::NEEDS_FILE:
        {
            if (outputPath.empty()) {
                usage(me);
                exit(1);
            }

            if (outputFormat->mOutputMode == OutputHandler::NEEDS_DIR) {
                if (outputPath.back() != '/') {
                    outputPath += "/";
                }
            }
            break;
        }
        case OutputHandler::NEEDS_SRC:
        {
            if (outputPath.empty()) {
                outputPath = coordinator.getRootPath();
            }
            if (outputPath.back() != '/') {
                outputPath += "/";
            }

            break;
        }

        default:
            outputPath.clear();  // Unused.
            break;
    }

    coordinator.addDefaultPackagePath("android.hardware", "hardware/interfaces");
    coordinator.addDefaultPackagePath("android.hidl", "system/libhidl/transport");
    coordinator.addDefaultPackagePath("android.frameworks", "frameworks/hardware/interfaces");
    coordinator.addDefaultPackagePath("android.system", "system/hardware/interfaces");

    for (int i = 0; i < argc; ++i) {
        FQName fqName(argv[i]);

        if (!fqName.isValid()) {
            fprintf(stderr,
                    "ERROR: Invalid fully-qualified name.\n");
            exit(1);
        }

        if (!outputFormat->validate(fqName, outputFormat->name())) {
            fprintf(stderr,
                    "ERROR: output handler failed.\n");
            exit(1);
        }

        status_t err =
            outputFormat->generate(fqName, me, &coordinator, outputPath);

        if (err != OK) {
            exit(1);
        }
    }

    return 0;
}
