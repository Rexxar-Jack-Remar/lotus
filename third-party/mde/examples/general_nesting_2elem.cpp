#include <iostream>

#include "mde/mde.hpp"

int main() {
	using Child1MDE =
		mde::MDENode<mde::MDEConfig<double>>;

	using Child1Index = typename Child1MDE::Index;

	using Child2MDE =
		mde::MDENode<mde::MDEConfig<double>>;

	using Child2Index = typename Child2MDE::Index;

	using MDE =
		mde::MDENode<
			mde::MDEConfig<double>,
			mde::NestingBase<int, Child1MDE, Child2MDE>>;

	using Index = typename MDE::Index;

	Child1MDE cl1;
	Child2MDE cl2;
	MDE l({cl1, cl2});

	Child1Index a1 = cl1.register_set_single(212.23);
	Child1Index b1 = cl1.register_set({22.2, 33.4, 33.122});
	Child1Index g1 = cl1.register_set({34.121});

	Child2Index a2 = cl2.register_set_single(212);
	Child2Index b2 = cl2.register_set({22, 33, 34});
	Child2Index g2 = cl2.register_set({34});

	Index c = l.register_set_single({12, {a1, a2}});
	Index f = l.register_set_single({12, {b1, b2}});
	Index e = l.register_set_single({12, {g1, g2}});

	l.set_union(c, f);
	l.set_intersection(c, f);
	l.set_intersection(f, e);
	l.set_intersection(c, f);

	l.set_difference(c, f);

	std::cout << cl1.dump() << std::endl;
	std::cout << cl1.dump_perf() << std::endl;
	std::cout << "======================" << std::endl;
	std::cout << cl2.dump() << std::endl;
	std::cout << cl2.dump_perf() << std::endl;
	std::cout << "======================" << std::endl;
	std::cout << l.dump() << std::endl;
	std::cout << l.dump_perf() << std::endl;

	return 0;
}