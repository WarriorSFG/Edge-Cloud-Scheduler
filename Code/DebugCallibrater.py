import Simulator
import Generator

# Generate a single test case
case = Generator.generate(
    1,
    seed=42,
    token_budget=Generator.TOKEN_BUDGET,
    max_R=Generator.MAX_R
)[0]

# Run it against your best scheduler
res = Simulator.run_one(case, binary="Schedulers/schedulerX", timeout_s=60.0)

print(f"Success: {res.ok}")
print(f"Score: {res.score}")
print(f"Violation Reason: {res.violation}")