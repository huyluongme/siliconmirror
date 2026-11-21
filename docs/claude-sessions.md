# Làm việc với Claude Code trên siliconmirror

Hướng dẫn cho chủ dự án: mở một session mới thế nào, nên gõ gì, và những
gì Claude tự lo. (Các file mà Claude đọc được viết bằng tiếng Anh, nằm
trong `CLAUDE.md` và `.claude/`.)

## 1. Mở session

1. Mở VS Code (hoặc terminal) ở **thư mục `siliconmirror`**, không phải
   thư mục cha. Claude Code chỉ tự nạp `CLAUDE.md` của thư mục đang làm
   việc và các thư mục cha.
2. Model và effort được chọn tự động từ `.claude/settings.json`: Opus,
   effort `high`. Không cần gõ gì.
3. Muốn chắc `CLAUDE.md` đã được nạp: gõ `/context` và tìm `CLAUDE.md`
   trong danh sách.

## 2. Các câu lệnh mẫu

Bắt đầu session (có thể bỏ qua, vì `CLAUDE.md` và `.claude/status.md` đã
được nạp sẵn):

```
Đọc CLAUDE.md và .claude/status.md, tóm tắt ngắn trạng thái hiện tại
và các task tiếp theo, rồi đợi tôi chọn task.
```

Chọn một task:

```
Làm task <số hoặc mô tả>. Đề xuất cách chia bước trước, đợi tôi đồng ý
rồi mới code. Xong thì dùng /finish-task.
```

Tiếp tục một task đang dở (session trước bị ngắt giữa chừng):

```
Session trước dừng giữa task <tên>. Xem git status, git diff --stat và
.claude/status.md, cho tôi biết đã làm tới đâu, rồi đề xuất bước tiếp.
```

Kết thúc một task (nếu Claude chưa tự làm):

```
/finish-task
```

Đồng ý cho commit (Claude chỉ commit khi bạn nói rõ như vậy):

```
đồng ý, commit và push đi
```

Thêm một board mới:

```
/add-board <tên board, ví dụ: Marvell Armada 3720 trên switch XYZ>
```

## 3. Effort: khi nào cần đổi

| Lúc nào | Gõ |
|---|---|
| Bình thường (tự động) | không gõ gì: `high` |
| Claude báo "chỗ này khó" (sửa hai lần vẫn lỗi, máy treo không rõ lý do, thiết kế CPU mới hoặc JIT, tài liệu mâu thuẫn) | `/effort xhigh` |
| Xong chỗ khó, hoặc Claude báo đã ổn | `/effort high` |
| Việc máy móc (tài liệu, boot log, phân loại mutant) | `/effort medium` (không bắt buộc) |

Claude không tự đổi effort được; nó chỉ báo, bạn gõ lệnh. Không nên đổi
model giữa session (mất cache, tốn thêm). Một session chỉ toàn việc máy
móc có thể mở bằng `claude --model sonnet` để tiết kiệm.

## 4. Những gì được cập nhật sau mỗi task

Không có phần mềm nào tự chạy ngầm: **chính Claude cập nhật**, ở bước 5
của `/finish-task`, rồi commit chung với task.

| File | Cập nhật khi nào |
|---|---|
| `.claude/status.md` | **mỗi task**: HEAD, task vừa xong, các task tiếp theo |
| `docs/boot-logs/` | mỗi task (boot log từ firmware tới Linux) |
| `docs/boards/<board>.md` | mỗi task có thay đổi về phần cứng |
| `.claude/knowledge/history.md` | khi xong một giai đoạn (một mục roadmap) |
| `.claude/knowledge/*.md` khác | khi có sự thật mới đã kiểm chứng mà code và docs chưa ghi |
| `CLAUDE.md`, `.claude/skills/`, `.claude/tools/` | ít khi: khi bạn đổi quy tắc hoặc cách làm. Chúng được viết dạng chung, dùng cho mọi task và mọi board |

Nếu một session kết thúc trước khi chạy `/finish-task`, `status.md` có
thể đã cũ. Lúc đó dùng câu "Tiếp tục một task đang dở" ở trên.

Muốn thêm hay đổi một quy tắc, nói với Claude, ví dụ: "ghi vào CLAUDE.md:
từ giờ ..." Claude sẽ sửa file và đợi bạn đồng ý commit.

## 5. Tiết kiệm token

- Mỗi task một session: session dài thì mỗi lượt đều gửi lại toàn bộ
  cuộc trò chuyện.
- Các file trong `.claude/knowledge/` chỉ được đọc khi cần, nên không tốn
  token ở đầu session.
- `sh .claude/tools/check.sh` in mỗi bước kiểm tra một dòng, thay vì cả
  trang kết quả.
