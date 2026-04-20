#include <iostream>

#include "mde/mde.hpp"

int main() {
	using ChildMDE =
		mde::MDENode<mde::MDEConfig<double>>;

	using ChildIndex = typename ChildMDE::Index;

	using MDE =
		mde::MDENode<
			mde::MDEConfig<int>,
			mde::NestingBase<int, ChildMDE>>;

	using Index = typename MDE::Index;

	ChildMDE cl;
	MDE l(MDE::RefList{cl});

	ChildIndex a = cl.register_set_single(212);
	std::set acc = {22, 33, 33};
	ChildIndex b = cl.register_set(acc.begin(), acc.end());
	ChildIndex g = cl.register_set({ 34 });

	Index c = l.register_set_single({12, {a}});
	Index f = l.register_set_single({12, {b}});
	Index e = l.register_set_single({12, {g}});

	l.set_union(c, f);
	l.set_intersection(c, f);
	l.set_intersection(f, e);
	l.set_intersection(c, f);

	l.set_difference(c, f);


	std::cout << cl.dump() << std::endl;
	std::cout << cl.dump_perf() << std::endl;
	std::cout << "======================" << std::endl;
	std::cout << l.dump() << std::endl;
	std::cout << l.dump_perf() << std::endl;

	return 0;
}