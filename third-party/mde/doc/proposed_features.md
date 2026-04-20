# Proposed Features

This document tracks proposed features that may be implemented in the future.

# MDE Configuration Struct
**Status: Implemented**

Instead of having inconveniently large number of template parameters for a
single MDE, we might encapsulate the parameters in a single struct. Users may
copy a skeleton of the struct and modify it for their own use.

```cpp
struct ConfigInt {
	using Property = int;
	using PropertyLess = mde::DefaultLess<int>;
	using PropertyHash = mde::DefaultHash<int>;
	using PropertyEqual = mde::DefaultEqual<int>;
	using PropertyPrinter  = mde::DefaultPrinter<int>;
};

// ...

using MDE = mde::MDENode<ConfigInt, NestingBase<ConfigInt, OtherMDE>>;
```