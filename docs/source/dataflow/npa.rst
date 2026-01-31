Newtonian Program Analysis (NPA)
================================

Overview
========

The **Newtonian Program Analysis (NPA)** engine in ``lib/Dataflow/NPA``
implements advanced, research-oriented techniques for compositional and
recurrence-based data-flow reasoning.
It is designed for **numeric** and **relational** analyses that go
beyond classical bit-vector or IFDS/IDE formulations.

* **Location**: ``lib/Dataflow/NPA`` (implementation), ``include/Dataflow/NPA`` (headers)
* **Purpose**: library infrastructure for Newton-style program analyses

Conceptual Background
=====================

NPA is based on a sequence of works that recast program analysis as a
form of **Newton iteration** over suitable abstract domains:

* *Newtonian Program Analysis*, JACM 2010 [EsparzaKieferLuttenberger2010]_.
* *Newtonian Program Analysis via Tensor Product*, POPL 2016.
* *Compositional Recurrence Analysis Revisited*, PLDI 2017.

The foundational paper [EsparzaKieferLuttenberger2010]_ presents a novel generic
technique for solving interprocedural dataflow equations by generalizing
Newton's method (the 300-year-old technique for computing zeros of differentiable
functions) to **ω-continuous semirings**.

Key Insight: Algebraic Generalization
--------------------------------------

The key insight is that Newton's method can be reformulated **purely algebraically**,
without requiring subtraction, division, or limits. This allows the method to be
applied to arbitrary semirings, not just the real numbers.

The method works as follows:

* Programs are encoded as systems of (possibly non-linear) recurrence equations
  over an ω-continuous semiring.
* At each iteration, Newton's method linearizes the system at the current
  approximation using **algebraic differentials**.
* The differential of a power series is defined inductively using algebraic rules
  (product rule, sum rule) rather than limits.
* Each Newton step solves a linear system to obtain the next approximation.

This yields:

* **Faster convergence** than classical Kleene iteration (often exponentially faster).
* **Robustness**: guaranteed convergence for any ω-continuous semiring.
* **Termination guarantees**: for idempotent commutative semirings, Newton's method
  terminates in at most *n* iterations for a system of *n* equations.

Mathematical Foundation
========================

ω-Continuous Semirings
----------------------

An **ω-continuous semiring** is a tuple :math:`\langle S, +, \cdot, 0, 1 \rangle` where:

* :math:`(S, +, 0)` is a commutative monoid (addition/join operation)
* :math:`(S, \cdot, 1)` is a monoid (multiplication/sequencing operation)
* Multiplication distributes over addition
* The relation :math:`a \sqsubseteq b` (defined as :math:`\exists d : a + d = b`) is a partial order
* Every ω-chain has a supremum with respect to :math:`\sqsubseteq`
* Infinite sums satisfy standard continuity properties

This generalizes:

* **Lattices** (classical dataflow analysis): idempotent semirings where :math:`a + a = a`
* **Language semirings**: languages over an alphabet with union and concatenation
* **Counting semirings**: sets of vectors (Parikh images) for counting occurrences
* **Probabilistic semirings**: nonnegative reals for probability and expected runtime analysis
* **Relational semirings**: relations over program states for may-alias and summary relations

Comparison with Kleene's Method
--------------------------------

Traditional dataflow analysis uses **Kleene iteration**, which:

* Starts at :math:`\kappa^{(0)} = 0` (or :math:`f(0)`)
* Iterates: :math:`\kappa^{(i+1)} = f(\kappa^{(i)})`
* Converges to the least fixed point :math:`\mu f = \sup_i \kappa^{(i)}`

**Newton's method** instead:

* Starts at :math:`\nu^{(0)} = f(0)`
* Iterates: :math:`\nu^{(i+1)} = \nu^{(i)} + \Delta^{(i)}` where :math:`\Delta^{(i)}` is the
  least solution of the linearized system :math:`Df|_{\nu^{(i)}}(X) + \delta^{(i)} = X`
* Converges at least as fast as Kleene: :math:`\kappa^{(i)} \sqsubseteq \nu^{(i)} \sqsubseteq \mu f`

For probabilistic programs, Newton's method can achieve linear convergence (one bit
of precision per iteration) compared to logarithmic convergence for Kleene iteration.
For commutative idempotent semirings, Newton's method guarantees termination in at
most *n* iterations for *n* equations.

Examples and Applications
=========================

The NPA framework supports both **qualitative** and **quantitative** analyses:

May-Alias Analysis
------------------

Using the **counting semiring** (Parikh abstraction), NPA can perform may-alias
analysis that tracks how many times each data access path is traversed. Unlike
Kleene iteration, which may not terminate for recursive procedures, Newton's method
terminates in one iteration for this class of problems.

Average Runtime Analysis
------------------------

Using **probabilistic semirings**, NPA can compute:
* The probability that a procedure terminates
* The expected runtime (conditional on termination)

For probabilistic programs with recursive procedures, Newton's method converges
substantially faster than Kleene iteration, achieving linear rather than logarithmic
convergence.

Other Applications
------------------

* **Language analysis**: Computing the set of execution paths (context-free languages)
* **Relational analysis**: Computing summary relations for interprocedural analysis
* **Cost analysis**: Expected resource consumption under probabilistic execution models

Distributive vs. Non-Distributive Analyses
===========================================

NPA supports both **distributive** and **non-distributive** program analyses:

* **Distributive analyses**: Multiplication distributes over addition
  (:math:`a \cdot (b + c) = a \cdot b + a \cdot c`). In this case, the least fixed point
  coincides with the join-over-all-paths (JOP) solution.

* **Non-distributive analyses**: Only subdistributivity holds
  (:math:`a \cdot (b + c) \sqsupseteq a \cdot b + a \cdot c`). In this case, the least
  fixed point is an **overapproximation** of the JOP solution, but still provides a
  sound analysis result. Examples include constant propagation analysis.

Usage Notes
===========

In the current code base, NPA is primarily exposed as an internal
library component.
Clients that use NPA are expected to be familiar with the above papers
and to instantiate the provided abstractions for their specific numeric
domains and recurrence schemes.

This engine is **not** currently wired into a dedicated command-line
tool; instead, it serves as a building block for experimental analyses
within Lotus.

References
==========

.. [EsparzaKieferLuttenberger2010] Javier Esparza, Stefan Kiefer, and Michael Luttenberger.
   Newtonian Program Analysis. Journal of the ACM, 57(6):1-47, 2010.

