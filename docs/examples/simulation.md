# A test that finds a race

An account two tasks withdraw from at once, a test that passes every time it
is run, and `shield test -sim`, which finds the order of events that breaks it
and the one line that brings that order back.

Uses: [tasks](../tour.md#tasks), [testing](../tour.md#testing),
[shared structures](../concurrency.md#shared-structures),
[simulation](../testing.md#simulation).

A package, `bank`, in a directory of its own:

```sword
// bank/bank.sword
package bank

error Insufficient = "not enough in the account"

struct Account {
    balance u64
}

func Open(amount u64) shared[Account] {
    return shared[Account](Account{balance: amount})
}

func Balance(a *shared[Account]) u64 {
    lock read acct := a {
        return acct.balance
    }
}

// Takes amount out, or fails when the account does not hold it.
func Withdraw(a *shared[Account], amount u64) !void {
    mut enough := false
    lock read acct := a {
        enough = acct.balance >= amount
    }
    if !enough {
        return error.Insufficient
    }
    lock acct := a {
        acct.balance -= amount
    }
}
```

And its test beside it:

```sword
// bank/bank_test.sword
package bank

import "std/testing"

func spend(a *shared[Account], amount u64, mut ok *bool) {
    Withdraw(a, amount) catch {
        return
    }
    *ok = true
}

func TestTwoWithdrawals(mut t *testing.T) !void {
    account := Open(100)
    mut first := false
    mut second := false
    scope {
        spawn spend(&account, 60, &first)
        spawn spend(&account, 60, &second)
    }
    t.Check(first != second)
    t.Equal(Balance(&account), u64(40))
}
```

## It passes

```
$ shield test ./bank
ok    1 tests  (0ms)
```

A hundred in the account, two withdrawals of sixty: one should go through and
the other be refused, leaving forty. Run the test and it passes. Run it two
hundred times and it passes two hundred times. Both tasks are short, and on a
real machine one of them is almost always finished before the other gets
going.

The race checker accepts the program, and is right to. Every access to the
account goes through `lock`, so no two tasks ever touch the balance at the
same moment. The bug is not a data race. It is in the order of things.

## It does not

```
$ shield test ./bank -sim
FAIL  TestTwoWithdrawals under seed 2
     panic: integer overflow at line 30
      reproduce: shield test -sim -seed 2 -run TestTwoWithdrawals ./bank
FAIL  1 tests, 1 failed, 100 seeds each
```

`-sim` runs the test once per seed, a hundred by default, on one thread, and at
every point where a task could give way to another, a `lock`, a `spawn`, the
end of a lock, the seed decides which runs next. Under seed 2 both tasks check
the balance, both see a hundred, and both go on to subtract sixty. The second
subtraction would take the balance below zero, and a `u64` cannot hold that,
so the program stops at line 30, `acct.balance -= amount`.

The last line of the failure is the whole of what it takes to see it again:

```
$ shield test -sim -seed 2 -run TestTwoWithdrawals ./bank
FAIL  TestTwoWithdrawals under seed 2
     panic: integer overflow at line 30
      reproduce: shield test -sim -seed 2 -run TestTwoWithdrawals ./bank
FAIL  1 tests, 1 failed, seed 2
```

The same seed makes the same choices, so the failure comes back every time,
and a bug that showed up once a month in production becomes one that can be
stepped through.

## The fix

The check and the change were two locks, with a gap between them where the
other task could spend the money. They have to be one:

```sword
// Takes amount out, or fails when the account does not hold it. The check
// and the change are one lock: nobody can spend the money in between.
func Withdraw(a *shared[Account], amount u64) !void {
    lock acct := a {
        if acct.balance < amount {
            return error.Insufficient
        }
        acct.balance -= amount
    }
}
```

A `return` inside the block lets go of the lock on the way out, so the early
failure needs nothing extra.

```
$ shield test ./bank -sim -seeds 5000
ok    1 tests, 5000 seeds each
```

Five thousand orders, and none of them breaks it. That is not a proof, but
each seed picks the order afresh, which explores a great deal more than running
the same test on the same machine five thousand times.
