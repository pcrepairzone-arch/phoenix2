- **First file read —** `$.!Boot/!Boot` **Obey script (561 bytes)** ✅
- VFS layer — FileCore driver registered, root cache populated
- WIMP, networking — scaffolded stubs

---

## 🙏 Acknowledgements

### David J Ruck

**Special thanks to David J Ruck** ([david.ruck@armclub.org.uk](mailto:david.ruck@armclub.org.uk)) for his invaluable help during the FileCore driver development. David's deep knowledge of RISC OS FileCore internals provided the critical breakthrough on chain traversal: the insight that `chain_offset` counts hops **from the END**of the chain, not the start. This single piece of knowledge unblocked weeks of debugging and is now documented in `docs/SBPr_Directory_Format.pdf`.

```c
/* David J Ruck's insight — chain_offset counts from END of chain */
if (chain_offset == 0 || chain_len <= chain_offset)
```
