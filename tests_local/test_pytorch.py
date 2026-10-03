import os
import sys
import time
import torch

def main():
    print("=" * 50)
    print("[PyTorch Test] XSched + PyTorch CUDA Workload Test")
    print("=" * 50)

    print(f"Python Version: {sys.version.split()[0]}")
    print(f"PyTorch Version: {torch.__version__}")
    print(f"CUDA Available: {torch.cuda.is_available()}")

    if not torch.cuda.is_available():
        print("ERROR: CUDA is not available in PyTorch!")
        sys.exit(1)

    device = torch.device("cuda:0")
    device_name = torch.cuda.get_device_name(device)
    print(f"Active Device: {device_name}")

    # 1. Warm-up
    print("\n[Step 1] Initializing CUDA stream & warm-up...")
    x = torch.randn(1024, 1024, device=device)
    y = torch.randn(1024, 1024, device=device)
    z = torch.matmul(x, y)
    torch.cuda.synchronize()
    print("  -> Warm-up complete.")

    # 2. Large Matrix Multiplication Benchmark
    N = 4096
    print(f"\n[Step 2] Running {N}x{N} Float32 Matrix Multiplications (5 iterations)...")
    a = torch.randn(N, N, device=device)
    b = torch.randn(N, N, device=device)

    start_time = time.perf_counter()
    for i in range(5):
        c = torch.matmul(a, b)
        torch.cuda.synchronize()
        print(f"  Iteration {i+1}/5 done.")
    elapsed = time.perf_counter() - start_time
    print(f"  -> Total computation time: {elapsed:.4f} s (Avg: {elapsed/5*1000:.2f} ms/iter)")

    # 3. Correctness check against CPU
    print("\n[Step 3] Verifying numerical accuracy against CPU reference...")
    c_cpu_ref = torch.matmul(a.cpu(), b.cpu())
    c_gpu_result = c.cpu()
    max_diff = torch.max(torch.abs(c_cpu_ref - c_gpu_result)).item()
    is_close = torch.allclose(c_cpu_ref, c_gpu_result, atol=1e-2, rtol=1e-3)
    print(f"  Max absolute difference: {max_diff:.6e} (TF32/FP32 tolerance acceptable: {is_close})")
    if is_close:
        print("  -> Numerical verification PASSED!")
    else:
        print("  -> WARNING: Numerical discrepancy detected!")
        sys.exit(1)

    print("\n" + "=" * 50)
    print("[PyTorch Test Result] ALL PYTORCH TESTS COMPLETED SUCCESSFULLY!")
    print("=" * 50)

if __name__ == "__main__":
    main()
