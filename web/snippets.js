// snippets.js: copy-ready client code for the local OpenAI-compatible API (#142).
// The official OpenAI SDKs are the language bindings; Java uses the JDK's own
// HttpClient so the snippet runs without a build tool. Shared by the Connect page
// and tests/app/snippets_test.py, which runs every snippet against a real service.
// ponytail: the key is written into the snippet, like the editor configurations;
// read it from the environment if the code leaves this computer.
(() => {
  const prompt = 'Say hello in one sentence.';
  const shell = text => `'${text.replaceAll("'", "'\\''")}'`;
  const str = text => JSON.stringify(text);
  const snippets = {
    terminal: (base, key, model) =>
      `curl ${shell(`${base}/chat/completions`)} -H ${shell(`Authorization: Bearer ${key}`)} -H 'Content-Type: application/json' --data ${shell(JSON.stringify({model, messages: [{role: 'user', content: 'Hello'}], max_tokens: 512}))}`,
    python: (base, key, model) => `# pip install openai
from openai import OpenAI

client = OpenAI(base_url=${str(base)}, api_key=${str(key)})
stream = client.chat.completions.create(
    model=${str(model)},
    messages=[{"role": "user", "content": ${str(prompt)}}],
    max_tokens=512,
    stream=True,
)
for chunk in stream:
    if chunk.choices:
        print(chunk.choices[0].delta.content or "", end="", flush=True)
print()
`,
    javascript: (base, key, model) => `// npm install openai  ·  save as hello.mjs, run: node hello.mjs
import OpenAI from "openai";

const client = new OpenAI({baseURL: ${str(base)}, apiKey: ${str(key)}});
const stream = await client.chat.completions.create({
  model: ${str(model)},
  messages: [{role: "user", content: ${str(prompt)}}],
  max_tokens: 512,
  stream: true,
});
for await (const chunk of stream) process.stdout.write(chunk.choices[0]?.delta?.content ?? "");
process.stdout.write("\\n");
`,
    go: (base, key, model) => `// go mod init hello && go get github.com/openai/openai-go  ·  go run .
package main

import (
	"context"
	"fmt"

	"github.com/openai/openai-go"
	"github.com/openai/openai-go/option"
)

func main() {
	client := openai.NewClient(option.WithBaseURL(${str(base + '/')}), option.WithAPIKey(${str(key)}))
	stream := client.Chat.Completions.NewStreaming(context.Background(), openai.ChatCompletionNewParams{
		Model:     ${str(model)},
		Messages:  []openai.ChatCompletionMessageParamUnion{openai.UserMessage(${str(prompt)})},
		MaxTokens: openai.Int(512),
	})
	for stream.Next() {
		if chunk := stream.Current(); len(chunk.Choices) > 0 {
			fmt.Print(chunk.Choices[0].Delta.Content)
		}
	}
	if err := stream.Err(); err != nil {
		panic(err)
	}
	fmt.Println()
}
`,
    java: (base, key, model) => `// Java 11+, no dependencies: save as Hello.java, run: java Hello.java
// (openai-java works too: OpenAIOkHttpClient.builder().baseUrl(...).apiKey(...))
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;

public class Hello {
    public static void main(String[] args) throws Exception {
        String body = ${str(JSON.stringify({model, messages: [{role: 'user', content: prompt}], max_tokens: 512}))};
        HttpRequest request = HttpRequest.newBuilder(URI.create(${str(`${base}/chat/completions`)}))
            .header("Authorization", "Bearer " + ${str(key)})
            .header("Content-Type", "application/json")
            .POST(HttpRequest.BodyPublishers.ofString(body))
            .build();
        HttpResponse<String> response = HttpClient.newHttpClient().send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) throw new RuntimeException(response.body());
        System.out.println(response.body());
    }
}
`
  };
  if (typeof module !== 'undefined') module.exports = snippets;
  else globalThis.connectionSnippets = snippets;
})();
