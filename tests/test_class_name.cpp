// extract_class_name: the class taken from std::source_location::function_name().
#include <gtest/gtest.h>

#include <string_view>

#include "lumen/detail/class_name.hpp"

namespace lumen {
namespace {

using detail::extract_class_name;

TEST(ClassNameTest, MethodYieldsItsClass) {
    EXPECT_EQ(extract_class_name("void MyClass::method()"), "MyClass");
}

TEST(ClassNameTest, StaticMethodYieldsItsClass) {
    EXPECT_EQ(extract_class_name("static void MyClass::static_method()"), "MyClass");
}

TEST(ClassNameTest, ConstructorYieldsItsClass) {
    EXPECT_EQ(extract_class_name("MyClass::MyClass(int)"), "MyClass");
}

TEST(ClassNameTest, NestedClassYieldsTheInnermostClass) {
    EXPECT_EQ(extract_class_name("void Outer::Inner::deep()"), "Inner");
}

TEST(ClassNameTest, ParameterTypesWithScopeDoNotConfuseIt) {
    EXPECT_EQ(extract_class_name("void Parser::parse(std::string_view, lumen::Core&)"), "Parser");
}

TEST(ClassNameTest, FreeFunctionYieldsEmpty) {
    EXPECT_EQ(extract_class_name("void free_function(int, double)"), "");
}

TEST(ClassNameTest, EmptyInputYieldsEmpty) {
    EXPECT_EQ(extract_class_name(""), "");
}

TEST(ClassNameTest, InputWithoutParenthesisYieldsEmpty) {
    EXPECT_EQ(extract_class_name("MyClass::member"), "");
}

TEST(ClassNameTest, ExtraSpaceBeforeTheQualifiedNameIsIgnored) {
    EXPECT_EQ(extract_class_name("void  A::b()"), "A");
}

TEST(ClassNameTest, NamespaceQualifiedFreeFunctionYieldsTheNamespace) {
    // A known limit of reading the text: a namespace looks like a class.
    EXPECT_EQ(extract_class_name("void ns::f()"), "ns");
}

TEST(ClassNameTest, WorksAtCompileTime) {
    static_assert(extract_class_name("void K::f()") == "K");
}

}  // namespace
}  // namespace lumen
